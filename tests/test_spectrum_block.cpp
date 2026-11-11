// Tests for the spectrum block (dsp/spectrum_block.hpp): windows against published figures, tone and noise level per window, the
// max-per-column reduction, fixed frame rate through overlap, hold modes, swap of size and window while the DSP thread runs, and
// "no allocation on the DSP thread". With FOXSDR_BLOCK_BENCH=1 it also prints the cost and memory table (not a pass/fail measurement).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/spectrum_block.hpp"

#include "alloc_probe.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace cascade::dsp;
using Cx = std::complex<float>;

namespace {

const SpecWindow kAllWindows[5] = {SpecWindow::Rectangular, SpecWindow::Hann, SpecWindow::Hamming, SpecWindow::BlackmanHarris,
                                   SpecWindow::FlatTop};
const char* wname(SpecWindow w) {
    switch (w) {
        case SpecWindow::Rectangular: return "rect";
        case SpecWindow::Hann: return "hann";
        case SpecWindow::Hamming: return "hamming";
        case SpecWindow::BlackmanHarris: return "bh4";
        case SpecWindow::FlatTop: return "flat";
    }
    return "?";
}

std::vector<Cx> tone(std::size_t n, double bin, double N, double amp = 1.0, double phase = 0.3) {
    std::vector<Cx> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = sbgen::kTwoPi * std::fmod(bin * static_cast<double>(i), N) / N + phase;  // fmod: exact phase for long runs
        v[i] = Cx(static_cast<float>(amp * std::cos(a)), static_cast<float>(amp * std::sin(a)));
    }
    return v;
}

// Complex white noise of total power p (re and im each p/2).
std::vector<Cx> cnoise(std::size_t n, double p, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> d(0.0, std::sqrt(p / 2.0));
    std::vector<Cx> v(n);
    for (auto& x : v) { x = Cx(static_cast<float>(d(g)), static_cast<float>(d(g))); }
    return v;
}

struct Lobe {
    double nullHalfWidthBins = 0, bw3Bins = 0, sidelobeDb = 0;
};

// Main-lobe and side-lobe figures of a size-n window from its zero-padded transform (16x).
Lobe measureWindow(SpecWindow w, std::size_t n) {
    const std::size_t pad = 16, M = n * pad;
    std::vector<float> win(n);
    specFillWindow(w, n, win.data());
    std::vector<Cx> in(M, Cx()), out(M);
    for (std::size_t i = 0; i < n; ++i) { in[i] = Cx(win[i], 0.0f); }
    ComplexFFT fft(M);
    fft.forward(in.data(), out.data());
    std::vector<double> mag(M / 2);
    for (std::size_t k = 0; k < M / 2; ++k) { mag[k] = std::abs(out[k]); }
    const double peak = mag[0];
    Lobe l;
    std::size_t k = 1;
    while (k + 1 < mag.size() && !(mag[k] <= mag[k - 1] && mag[k] <= mag[k + 1])) { ++k; }
    l.nullHalfWidthBins = static_cast<double>(k) / pad;
    // -3 dB half width by linear interpolation
    const double thr = peak / std::sqrt(2.0);
    for (std::size_t j = 1; j < mag.size(); ++j) {
        if (mag[j] < thr) {
            const double f = (mag[j - 1] - thr) / (mag[j - 1] - mag[j]);
            l.bw3Bins = 2.0 * (static_cast<double>(j - 1) + f) / pad;
            break;
        }
    }
    double side = 0;
    for (std::size_t j = k; j < mag.size(); ++j) { side = std::max(side, mag[j]); }
    l.sidelobeDb = 20.0 * std::log10(side / peak);
    return l;
}

// Peak (max) of one frame of the block for a tone input, plus where it sits.
float peakOf(const float* db, std::size_t n, std::size_t* at = nullptr) {
    std::size_t bi = 0;
    for (std::size_t i = 1; i < n; ++i) { if (db[i] > db[bi]) { bi = i; } }
    if (at != nullptr) { *at = bi; }
    return db[bi];
}

SpecConfig baseCfg(std::size_t n, SpecWindow w) {
    SpecConfig c;
    c.fftSize = n;
    c.window = w;
    c.sampleRateHz = 48000.0 * 50;  // 2.4 MS/s
    c.hop = n;                       // clamped up to n/16 only; n is legal
    c.mode = SpecMode::Instant;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------------------
void testWindowFigures() {
    struct Pub { SpecWindow w; double enbw, enbwTol, null, side, sideTol; };
    // enbw / sidelobe: Keysight 89600B help "Window Shapefactor and Equivalent Noise Bandwidth" (Hann 1.50 / -31, BH4 -92, rect 1.00 / -13);
    // the rect side lobe is the analytic Dirichlet value -13.26 dB; Hamming (0.54/0.46) and flat-top numbers are the measured ones, pinned.
    const Pub pub[5] = {{SpecWindow::Rectangular, 1.0, 1e-6, 1.0, -13.26, 0.1},
                        {SpecWindow::Hann, 1.5, 1e-4, 2.0, -31.5, 0.3},
                        {SpecWindow::Hamming, 1.3628, 2e-3, 2.0, -42.7, 0.5},
                        {SpecWindow::BlackmanHarris, 2.0044, 2e-3, 4.0, -92.0, 1.0},
                        {SpecWindow::FlatTop, 3.7702, 5e-3, 5.0, -93.0, 3.0}};
    for (const Pub& p : pub) {
        const std::size_t n = 1024;
        std::vector<float> w(n);
        specFillWindow(p.w, n, w.data());
        const SpecWindowStats st = specWindowStats(w.data(), n);
        const double enbw = static_cast<double>(n) * st.sumSq / (st.sum * st.sum);
        const Lobe l = measureWindow(p.w, n);
        std::printf("window %-8s CG %.5f ENBW %.4f null +-%.2f bins  3dB width %.3f bins  highest side lobe %.2f dB\n", wname(p.w),
                    st.sum / static_cast<double>(n), enbw, l.nullHalfWidthBins, l.bw3Bins, l.sidelobeDb);
        CHECK_NEAR(enbw, p.enbw, p.enbwTol);
        CHECK_NEAR(l.nullHalfWidthBins, p.null, 0.07);
        CHECK_NEAR(l.sidelobeDb, p.side, p.sideTol);
    }
    // coherent gains from the coefficients
    std::vector<float> w(1024);
    specFillWindow(SpecWindow::Hann, 1024, w.data());
    CHECK_NEAR(specWindowStats(w.data(), 1024).sum / 1024.0, 0.5, 1e-6);
    specFillWindow(SpecWindow::BlackmanHarris, 1024, w.data());
    CHECK_NEAR(specWindowStats(w.data(), 1024).sum / 1024.0, 0.35875, 1e-6);
    specFillWindow(SpecWindow::FlatTop, 1024, w.data());
    CHECK_NEAR(specWindowStats(w.data(), 1024).sum / 1024.0, 0.21557895, 1e-6);
    // periodic: w[0] is the minimum, no symmetric end point
    specFillWindow(SpecWindow::Hann, 1024, w.data());
    CHECK(w[0] == 0.0f && w[512] > 0.9999f && w[1023] > 0.0f);
    // 3 dB widths pinned to what was measured (bins): rect 0.89, hann 1.44, hamming 1.30, bh4 1.90 are the textbook values
    CHECK_NEAR(measureWindow(SpecWindow::Rectangular, 1024).bw3Bins, 0.89, 0.02);
    CHECK_NEAR(measureWindow(SpecWindow::Hann, 1024).bw3Bins, 1.44, 0.02);
    CHECK_NEAR(measureWindow(SpecWindow::Hamming, 1024).bw3Bins, 1.30, 0.02);
    CHECK_NEAR(measureWindow(SpecWindow::BlackmanHarris, 1024).bw3Bins, 1.90, 0.03);
}

// A single-bin tone at every size and window: unreduced and reduced, within 0.5 dB of the true level.
void testToneAtEverySize() {
    float worstFull = 0, worstRed = 0;
    for (std::size_t n = kSpecMinFft; n <= kSpecMaxFft; n *= 2) {
        for (SpecWindow w : kAllWindows) {
            if (n > 65536 && w != SpecWindow::BlackmanHarris && w != SpecWindow::Rectangular) { continue; }  // time: every window to 64k
            SpecConfig c = baseCfg(n, w);
            SpectrumBlock full(c);
            c.columns = 1000;  // does not divide any size
            SpectrumBlock red(c);
            const std::size_t bin = n / 2 + 7 + n / 8;  // a positive-frequency bin, not DC
            const std::vector<Cx> x = tone(n, static_cast<double>(bin - n / 2), static_cast<double>(n), 0.7);
            full.process(x.data(), n);
            red.process(x.data(), n);
            std::vector<float> a(n), b(1000);
            const std::size_t na = full.latest(a.data(), a.size());
            const std::size_t nb = red.latest(b.data(), b.size());
            CHECK(na == n && nb == 1000);
            std::size_t at = 0;
            const float pf = peakOf(a.data(), na, &at);
            const float pr = peakOf(b.data(), nb);
            const float want = 20.0f * std::log10(0.7f);
            worstFull = std::max(worstFull, std::fabs(pf - want));
            worstRed = std::max(worstRed, std::fabs(pr - want));
            CHECK(at == bin);
            CHECK(std::fabs(pf - want) < 0.05f);
            CHECK(std::fabs(pr - want) < 0.5f);
            CHECK(std::fabs(pr - pf) < 1e-4f);  // max-per-column loses nothing
        }
    }
    std::printf("tone level error worst: full %.4f dB, reduced %.4f dB\n", worstFull, worstRed);
    // A tone between bins: scalloping per window (flat-top almost none, Hann up to 1.4 dB); the maths the display user lives with.
    float sc[5];
    int i = 0;
    for (SpecWindow w : kAllWindows) {
        SpectrumBlock b(baseCfg(4096, w));
        const std::vector<Cx> x = tone(4096, 1000.5, 4096.0, 1.0);
        b.process(x.data(), 4096);
        std::vector<float> o(4096);
        b.latest(o.data(), o.size());
        sc[i++] = -peakOf(o.data(), 4096);
    }
    std::printf("scalloping loss at half a bin: rect %.2f hann %.2f hamming %.2f bh4 %.2f flat %.3f dB\n", sc[0], sc[1], sc[2], sc[3], sc[4]);
    CHECK_NEAR(sc[0], 3.92, 0.05);  // sinc(0.5)
    CHECK_NEAR(sc[1], 1.42, 0.05);
    CHECK(sc[4] < 0.1f);            // flat-top: the reason it exists
}

void testNoiseLevel() {
    const std::size_t n = 65536;
    const double fs = 2400000.0;
    for (SpecWindow w : kAllWindows) {
        std::vector<float> win(n);
        specFillWindow(w, n, win.data());
        const SpecWindowStats st = specWindowStats(win.data(), n);
        const double enbw = static_cast<double>(n) * st.sumSq / (st.sum * st.sum);
        const double P = 0.25;
        const std::vector<Cx> x = cnoise(n, P, 5);
        double meanPsd = 0, meanTone = 0;
        for (SpecScale sc : {SpecScale::Psd, SpecScale::Tone}) {
            SpecConfig c = baseCfg(n, w);
            c.scale = sc;
            c.sampleRateHz = fs;
            SpectrumBlock b(c);
            b.process(x.data(), n);
            std::vector<float> o(n);
            b.latest(o.data(), n);
            double s = 0;
            for (float v : o) { s += std::pow(10.0, v / 10.0); }
            (sc == SpecScale::Psd ? meanPsd : meanTone) = 10.0 * std::log10(s / static_cast<double>(n));
        }
        // PSD: P/fs per Hz. Tone scale: P * ENBW / N per bin (the noise in one bin's equivalent bandwidth, relative to a full-scale tone).
        const double wantPsd = 10.0 * std::log10(P / fs);
        const double wantTone = 10.0 * std::log10(P * enbw / static_cast<double>(n));
        std::printf("noise %-8s psd %.3f (want %.3f)  tone-scale %.3f (want %.3f)\n", wname(w), meanPsd, wantPsd, meanTone, wantTone);
        CHECK_NEAR(meanPsd, wantPsd, 0.1);
        CHECK_NEAR(meanTone, wantTone, 0.1);
    }
}

void testReduce() {
    // partition: every bin belongs to exactly one column, so the maximum of the whole survives
    for (std::size_t n : {1024u, 1000u, 4096u, 65536u, 7u}) {
        for (std::size_t cols : {1u, 3u, 7u, 100u, 999u, 1024u, 2000u}) {
            std::vector<float> in(n, -100.0f);
            for (std::size_t hot : {std::size_t(0), n / 3, n - 1}) {
                std::fill(in.begin(), in.end(), -100.0f);
                in[hot] = 5.0f;
                std::vector<float> out(cols, -999.0f);
                specReduceMax(in.data(), n, out.data(), cols);
                int fives = 0;
                for (float v : out) { if (v == 5.0f) { ++fives; } }
                CHECK(fives >= 1);
                CHECK(*std::max_element(out.begin(), out.end()) == 5.0f);
                for (float v : out) { CHECK(v == 5.0f || v == -100.0f); }
            }
        }
    }
    // exact small case
    const float in[10] = {1, 9, 3, 4, 5, 6, 7, 2, 8, 0};
    float out[3];
    specReduceMax(in, 10, out, 3);  // [0,3) [3,6) [6,10)
    CHECK(out[0] == 9 && out[1] == 6 && out[2] == 8);
    // zero sizes do nothing
    float sentinel = 42;
    specReduceMax(in, 0, &sentinel, 1);
    specReduceMax(in, 10, &sentinel, 0);
    CHECK(sentinel == 42);
    // columns >= n replicate (no information is invented, none lost)
    float up[20];
    specReduceMax(in, 10, up, 20);
    CHECK(up[0] == 1 && up[1] == 1 && up[2] == 9 && up[19] == 0);
}

void testFrameRate() {
    const double fs = 2400000.0;
    // fixed 30 fps across all sizes by overlap
    for (std::size_t n = kSpecMinFft; n <= kSpecMaxFft; n *= 2) {
        SpecConfig c;
        c.fftSize = n;
        c.sampleRateHz = fs;
        c.frameRateHz = 30.0;
        c.hop = 0;
        c.window = SpecWindow::Hann;
        SpectrumBlock b(c);
        const SpecPlan p = specPlan(fs, n, 30.0);
        CHECK(p.hop == 80000);
        CHECK_NEAR(p.frameRateHz, 30.0, 1e-9);
        std::vector<Cx> chunk(48000, Cx(0.1f, 0.0f));
        std::uint64_t f0 = 0;
        for (int i = 0; i < 100; ++i) {  // 2 s
            b.process(chunk.data(), chunk.size());
            if (i == 49) { f0 = b.framesProduced(); }
        }
        const std::uint64_t f1 = b.framesProduced() - f0;
        CHECK(f1 >= 29 && f1 <= 31);  // the second second of input
    }
    // the plan caps the rate when the overlap would exceed 15/16, and reports it
    const SpecPlan cap = specPlan(fs, 524288, 100.0);
    CHECK(cap.hop == 32768 && std::fabs(cap.overlap - 0.9375) < 1e-9 && cap.frameRateHz < 100.0);
    const SpecPlan hi = specPlan(10e6, 1024, 30.0);
    CHECK(hi.hop == 333333 && hi.overlap < 0);  // gaps between frames, said so
    // chunking does not change the frames: the last frame is bit-identical for any split
    const std::size_t n = 2048;
    SpecConfig c = baseCfg(n, SpecWindow::Hann);
    c.hop = 512;
    const std::vector<Cx> x = cnoise(20000, 1.0, 9);
    SpectrumBlock a(c), b(c);
    a.process(x.data(), x.size());
    std::size_t i = 0, k = 1;
    while (i < x.size()) {
        const std::size_t m = std::min(k, x.size() - i);
        b.process(x.data() + i, m);
        i += m;
        k = k * 3 + 1;
        if (k > 5000) { k = 1; }
    }
    std::vector<float> fa(n), fb(n);
    CHECK(a.framesProduced() == b.framesProduced() && a.framesProduced() > 20);
    a.latest(fa.data(), n);
    b.latest(fb.data(), n);
    CHECK(std::memcmp(fa.data(), fb.data(), n * sizeof(float)) == 0);
    // no frame before the FFT has a full window of input
    SpectrumBlock e(baseCfg(1024, SpecWindow::Hann));
    std::vector<Cx> few(1023, Cx(1, 0));
    e.process(few.data(), few.size());
    e.process(few.data(), 0);
    CHECK(e.framesProduced() == 0);
    std::vector<float> o(1024);
    CHECK(e.latest(o.data(), 1024) == 0);
    e.process(few.data(), 1);
    CHECK(e.framesProduced() == 1);
    CHECK(e.latest(o.data(), 1024) == 1024);
    CHECK(e.latest(o.data(), 1024) == 0);  // nothing new
}

void testHolds() {
    const std::size_t n = 1024;
    auto run = [&](SpecMode m, float alpha, float decay, const std::vector<std::vector<Cx>>& frames, std::vector<float>& last) {
        SpecConfig c = baseCfg(n, SpecWindow::Rectangular);
        c.mode = m;
        c.alpha = alpha;
        c.peakDecayDbPerS = decay;
        SpectrumBlock b(c);
        for (const auto& f : frames) { b.process(f.data(), f.size()); }
        last.assign(n, 0.0f);
        b.latest(last.data(), n);
    };
    const std::vector<Cx> loud = tone(n, 100, n, 1.0), quiet = tone(n, 100, n, 0.1), other = tone(n, 300, n, 0.5);
    std::vector<float> o;
    const std::size_t b100 = n / 2 + 100, b300 = n / 2 + 300;
    // peak hold keeps the loud frame after the signal is gone
    run(SpecMode::PeakHold, 1, 0, {loud, quiet, quiet}, o);
    CHECK_NEAR(o[b100], 0.0f, 0.01);
    // instant follows
    run(SpecMode::Instant, 1, 0, {loud, quiet}, o);
    CHECK_NEAR(o[b100], -20.0f, 0.01);
    // min hold: a bin that is loud in one frame and quiet in another reads the quiet one; a tone present in one frame only reads (near) nothing
    run(SpecMode::MinHold, 1, 0, {loud, quiet}, o);
    CHECK_NEAR(o[b100], -20.0f, 0.01);
    run(SpecMode::MinHold, 1, 0, {other, loud}, o);
    CHECK(o[b300] < -120.0f);  // 300 was only in the first frame: the minimum is the empty bin
    run(SpecMode::PeakHold, 1, 0, {other, loud}, o);
    CHECK_NEAR(o[b300], 20.0f * std::log10(0.5f), 0.01);
    // average: alpha 0.5 over loud, quiet -> power 0.5*1 + 0.5*0.01 = 0.505 (first frame primes)
    run(SpecMode::Average, 0.5f, 0, {loud, quiet}, o);
    CHECK_NEAR(o[b100], 10.0f * std::log10(0.505f), 0.01);
    // average of many frames of noise has far less spread than one frame (the point of averaging)
    std::vector<std::vector<Cx>> many;
    for (int i = 0; i < 40; ++i) { many.push_back(cnoise(n, 1.0, 100 + i)); }
    std::vector<float> avg, inst;
    run(SpecMode::Average, 0.1f, 0, many, avg);
    run(SpecMode::Instant, 1, 0, many, inst);
    auto sd = [](const std::vector<float>& v) {
        double m = 0, s = 0;
        for (float x : v) { m += x; }
        m /= static_cast<double>(v.size());
        for (float x : v) { s += (x - m) * (x - m); }
        return std::sqrt(s / static_cast<double>(v.size()));
    };
    std::printf("noise spread (std of dB over bins): instant %.2f, averaged %.2f\n", sd(inst), sd(avg));
    CHECK(sd(avg) < 0.6 * sd(inst));
    // peak decay: 100 dB/s with hop = n at 2.4 MS/s: 1024/2.4e6 s per frame = 0.4267 ms -> 0.0427 dB per frame; run 2000 frames
    {
        SpecConfig c = baseCfg(n, SpecWindow::Rectangular);
        c.mode = SpecMode::PeakHold;
        c.peakDecayDbPerS = 100.0f;
        SpectrumBlock b(c);
        b.process(loud.data(), n);
        for (int i = 0; i < 2000; ++i) { b.process(quiet.data(), n); }
        std::vector<float> r(n);
        b.latest(r.data(), n);
        const double secs = 2000.0 * n / 2400000.0;
        const double want = std::max(-20.0, -100.0 * secs);  // decays toward the new level, never below it
        CHECK_NEAR(r[b100], want, 0.3);
    }
    // a mode change restarts the hold
    SpecConfig c = baseCfg(n, SpecWindow::Rectangular);
    c.mode = SpecMode::PeakHold;
    SpectrumBlock b(c);
    b.process(loud.data(), n);
    b.setMode(SpecMode::PeakHold, 0.3f, 0.0f);
    b.resetHolds();
    b.process(quiet.data(), n);
    std::vector<float> r(n);
    b.latest(r.data(), n);
    CHECK_NEAR(r[b100], -20.0f, 0.01);
}

void testInvalid() {
    for (std::size_t bad : {std::size_t(0), std::size_t(512), std::size_t(1000), std::size_t(3072), std::size_t(1048576)}) {
        SpecConfig c;
        c.fftSize = bad;
        CHECK(SpectrumBlock::prepare(c) == nullptr);
        bool threw = false;
        try { SpectrumBlock b(c); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
    SpectrumBlock b(baseCfg(1024, SpecWindow::Hann));
    { SpecConfig bad; bad.fftSize = 1536; CHECK(!b.reconfigure(bad)); }
    CHECK(b.fftSize() == 1024);
    // columns above kSpecMaxColumns are capped; columns above fftSize mean full
    SpecConfig c = baseCfg(65536, SpecWindow::Hann);
    c.columns = 60000;
    CHECK(SpectrumBlock::prepare(c)->columns() == kSpecMaxColumns);
    c.columns = 70000;
    CHECK(SpectrumBlock::prepare(c)->columns() == 65536);
    // set-up in the engine: alpha and decay are clamped
    b.setMode(SpecMode::Average, 5.0f, -3.0f);
    b.setMode(SpecMode::Average, -1.0f, 0.0f);
}

void testNoAllocation() {
    for (SpecMode m : {SpecMode::Instant, SpecMode::Average, SpecMode::PeakHold, SpecMode::MinHold}) {
        for (std::size_t cols : {std::size_t(0), std::size_t(500)}) {
            SpecConfig c = baseCfg(8192, SpecWindow::FlatTop);
            c.mode = m;
            c.columns = cols;
            c.hop = 2048;
            SpectrumBlock b(c);
            const std::vector<Cx> x = cnoise(100000, 1.0, 3);
            std::vector<float> o(8192);
            b.process(x.data(), 20000);  // warm
            AllocProbe probe;
            for (int i = 0; i < 20; ++i) { b.process(x.data(), x.size()); }
            b.setMode(m, 0.2f, 1.0f);
            b.resetHolds();
            const long news = probe.news();
            CHECK(news == 0);
            CHECK(b.framesProduced() > 500);
        }
    }
}

void testSwap() {
    const int liveBefore = SpectrumBlock::Engine::live().load();
    {
        SpectrumBlock b(baseCfg(1024, SpecWindow::Hann));
        const std::vector<Cx> x = tone(4096, 100, 1024, 1.0);
        b.process(x.data(), x.size());
        CHECK(b.fftSize() == 1024);
        SpecConfig c = baseCfg(16384, SpecWindow::FlatTop);
        c.columns = 800;
        auto e = SpectrumBlock::prepare(c);  // all the allocation and planning happens here, not on the DSP thread
        CHECK(e != nullptr);
        b.swapIn(std::move(e));
        CHECK(b.fftSize() == 1024);  // not adopted until the DSP thread runs
        const std::vector<Cx> y = tone(40000, 200, 16384, 1.0);
        {
            AllocProbe probe;
            b.process(y.data(), y.size());  // adopts and runs: zero allocation including the swap
            CHECK(probe.news() == 0);
        }
        CHECK(b.fftSize() == 16384 && b.columns() == 800);
        std::vector<float> o(800);
        CHECK(b.latest(o.data(), 800) == 800);
        CHECK_NEAR(peakOf(o.data(), 800), 0.0f, 0.5);
        // two swaps before adoption: the first is freed by the control thread, the second adopted
        b.swapIn(SpectrumBlock::prepare(baseCfg(2048, SpecWindow::Hann)));
        b.swapIn(SpectrumBlock::prepare(baseCfg(4096, SpecWindow::Hamming)));
        b.process(y.data(), 10);
        CHECK(b.fftSize() == 4096);
        b.collectRetired();
    }
    CHECK(SpectrumBlock::Engine::live().load() == liveBefore);  // nothing leaked or double-freed
}

// DSP thread runs flat out while the control thread swaps sizes and windows and reads frames.
void testSwapConcurrent() {
    const int liveBefore = SpectrumBlock::Engine::live().load();
    {
        SpectrumBlock b(baseCfg(1024, SpecWindow::Hann));
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> processed{0};
        const std::vector<Cx> x = tone(30000, 500, 1024, 1.0);
        std::thread dsp([&] {
            while (!stop.load()) {
                b.process(x.data(), x.size());
                processed.fetch_add(1);
            }
        });
        const std::size_t sizes[4] = {1024, 4096, 16384, 2048};
        int readOk = 0;
        for (int i = 0; i < 300; ++i) {
            SpecConfig c = baseCfg(sizes[i & 3], kAllWindows[i % 5]);
            c.columns = (i % 3 == 0) ? 640 : 0;
            c.hop = 0;
            c.frameRateHz = 200.0;
            b.reconfigure(c);
            std::vector<float> o(16384);
            const std::size_t got = b.latest(o.data(), o.size());
            if (got != 0) {
                ++readOk;
                CHECK(std::isfinite(peakOf(o.data(), got)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop = true;
        dsp.join();
        b.collectRetired();
        CHECK(processed.load() > 10);
        CHECK(readOk > 20);
        std::printf("concurrent swap: %d reconfigures, %d frames read, %llu process calls\n", 300, readOk,
                    static_cast<unsigned long long>(processed.load()));
    }
    CHECK(SpectrumBlock::Engine::live().load() == liveBefore);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Cost and memory (FOXSDR_BLOCK_BENCH=1). Prints a table; checks nothing about speed.
std::size_t workingSet() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.WorkingSetSize;
#else
    return 0;
#endif
}

void bench() {
    std::printf("BENCH: cost per frame and CPU share of one core, window BH4, Average, 30 fps wanted (hop floor N/16), 1000 display columns\n");
    std::printf("%-8s | %-12s | %-9s | %-8s | %-9s | %-8s | %-9s\n", "size", "ms/frame 2.4M", "load 2.4M", "fps", "ms/f 10M", "load 10M",
                "fps10M");
    for (std::size_t n = kSpecMinFft; n <= kSpecMaxFft; n *= 2) {
        double ms[2] = {0, 0}, load[2] = {0, 0}, fps[2] = {0, 0};
        int idx = 0;
        for (double fs : {2400000.0, 10000000.0}) {
            SpecConfig c;
            c.fftSize = n;
            c.window = SpecWindow::BlackmanHarris;
            c.sampleRateHz = fs;
            c.frameRateHz = 30.0;
            c.hop = 0;
            c.columns = 1000;
            c.mode = SpecMode::Average;
            SpectrumBlock b(c);
            const SpecPlan p = specPlan(fs, n, 30.0);
            const std::size_t chunk = 65536;
            std::vector<Cx> x(chunk, Cx(0.1f, 0.05f));
            // fill once, then time a number of frames of input
            const std::size_t warm = n + 2 * p.hop;
            for (std::size_t d = 0; d < warm; d += chunk) { b.process(x.data(), chunk); }
            const std::uint64_t f0 = b.framesProduced();
            const std::size_t want = std::max<std::size_t>(8 * p.hop, 4 * n);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t d = 0; d < want; d += chunk) { b.process(x.data(), chunk); }
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double frames = static_cast<double>(b.framesProduced() - f0);
            // whole cost per second of input = s * fs / samplesFed
            const double perSecond = s * fs / static_cast<double>(((want + chunk - 1) / chunk) * chunk);
            ms[idx] = frames > 0 ? 1000.0 * s / frames : 0;
            load[idx] = 100.0 * perSecond;
            fps[idx] = p.frameRateHz;
            ++idx;
        }
        std::printf("%-8zu | %-12.3f | %-8.1f%% | %-8.1f | %-9.3f | %-7.1f%% | %-8.1f\n", n, ms[0], load[0], fps[0], ms[1], load[1], fps[1]);
    }
    std::printf("BENCH memory: engine bytes counted from the block's own buffers, and the process working-set growth for one block\n");
    std::printf("%-8s | %-14s | %-16s\n", "size", "counted bytes", "working-set delta");
    for (std::size_t n = kSpecMinFft; n <= kSpecMaxFft; n *= 2) {
        const std::size_t before = workingSet();
        SpecConfig c;
        c.fftSize = n;
        c.columns = 1000;
        SpectrumBlock b(c);
        std::vector<Cx> x(n, Cx(0.1f, 0.0f));
        b.process(x.data(), n);
        b.process(x.data(), n);
        const std::size_t after = workingSet();
        std::printf("%-8zu | %-14zu | %-16zu\n", n, b.engine().memoryBytes(), after > before ? after - before : 0);
    }
}

}  // namespace

int main() {
    testWindowFigures();
    testToneAtEverySize();
    testNoiseLevel();
    testReduce();
    testFrameRate();
    testHolds();
    testInvalid();
    testNoAllocation();
    testSwap();
    testSwapConcurrent();
    if (std::getenv("FOXSDR_BLOCK_BENCH") != nullptr) { bench(); }
    return testSummary("test_spectrum_block");
}
