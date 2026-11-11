// scope_analysis.cpp - see scope_analysis.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "export_logic.hpp"
#include "spectrum_extras.hpp"

namespace foxsdr::slintscope {

namespace {
constexpr float kPi = 3.14159265358979f;
const Rgba kBg{0x04, 0x0a, 0x06, 255}, kGrid{0x1c, 0x3a, 0x28, 255}, kBeam{0x5a, 0xff, 0x9a, 255}, kText{0xb4, 0xff, 0xcc, 255};

void text(Rgba* px, int w, int h, int x, int y, const std::string& t) {
    foxsdr::slintexport::drawLabelRgba(reinterpret_cast<std::uint8_t*>(px), static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), x, y, t, kText.r, kText.g, kText.b, 255, 1);
}
std::string f1(const char* f, double a) { char b[64]; std::snprintf(b, sizeof(b), f, a); return b; }
void graticule(Canvas& cv, int w, int h) {
    cv.fill(kBg);
    for (int i = 1; i < 10; ++i) { cv.line(static_cast<float>(i * w) / 10.0f, 0, static_cast<float>(i * w) / 10.0f, static_cast<float>(h), 1.0f, Rgba{kGrid.r, kGrid.g, kGrid.b, 160}); }
    for (int i = 1; i < 8; ++i) { cv.line(0, static_cast<float>(i * h) / 8.0f, static_cast<float>(w), static_cast<float>(i * h) / 8.0f, 1.0f, Rgba{kGrid.r, kGrid.g, kGrid.b, 160}); }
}
}  // namespace

void autocorrelation(const float* x, std::size_t n, std::size_t maxLag, std::vector<float>& out) {
    out.assign(maxLag + 1, 0.0f);
    if (x == nullptr || n < 4) { return; }
    double mean = 0;
    for (std::size_t i = 0; i < n; ++i) { mean += x[i]; }
    mean /= static_cast<double>(n);
    std::vector<float> v(n);
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(x[i] - mean); e += static_cast<double>(v[i]) * v[i]; }
    if (!(e > 1e-18)) { return; }
    maxLag = std::min(maxLag, n - 1);
    for (std::size_t k = 0; k <= maxLag; ++k) {
        double s = 0;
        for (std::size_t i = 0; i + k < n; ++i) { s += static_cast<double>(v[i]) * v[i + k]; }
        out[k] = static_cast<float>(s / e);
    }
}

std::size_t autocorrelationPeak(const std::vector<float>& r, float minHeight) {
    std::size_t k = 1;
    while (k < r.size() && r[k] > 0.0f) { ++k; }   // out of the first lobe
    std::size_t best = 0;
    float bestV = minHeight;
    for (; k + 1 < r.size(); ++k) { if (r[k] > bestV && r[k] >= r[k - 1] && r[k] >= r[k + 1]) { bestV = r[k]; best = k; } }
    return best;
}

void cepstrum(const std::vector<float>& spectrumDb, std::size_t maxQ, std::vector<float>& out) {
    out.assign(maxQ + 1, 0.0f);
    const std::size_t m = spectrumDb.size();
    if (m < 8) { return; }
    const std::size_t nfft = 2 * m;
    static std::vector<float> cosTable;
    if (cosTable.size() != nfft) { cosTable.resize(nfft); for (std::size_t i = 0; i < nfft; ++i) { cosTable[i] = std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(nfft)); } }
    double mean = 0;
    for (const float d : spectrumDb) { mean += d; }
    mean /= static_cast<double>(m);
    for (std::size_t q = 0; q <= maxQ; ++q) {
        double s = 0;
        for (std::size_t k = 0; k < m; ++k) { s += (static_cast<double>(spectrumDb[k]) - mean) * cosTable[(k * q) % nfft]; }
        out[q] = static_cast<float>(s / static_cast<double>(m) / 20.0);   // dB to a natural-ish log amplitude scale
    }
}

std::size_t cepstrumPeak(const std::vector<float>& c, std::size_t qLo, std::size_t qHi, float minAbove) {
    if (c.empty() || qLo >= c.size()) { return 0; }
    qHi = std::min(qHi, c.size() - 1);
    if (qLo + 2 > qHi) { return 0; }
    double mean = 0;
    std::size_t best = qLo;
    for (std::size_t q = qLo; q <= qHi; ++q) { mean += c[q]; if (c[q] > c[best]) { best = q; } }
    mean /= static_cast<double>(qHi - qLo + 1);
    if (c[best] - mean < minAbove) { return 0; }
    // a harmonic series has peaks at the period and at every multiple of it, and noise may make a multiple the tallest: the FIRST local peak that reaches 60 % of the tallest (over the mean) is the period
    for (std::size_t q = qLo + 1; q < best; ++q) {
        if (c[q] >= c[q - 1] && c[q] >= c[q + 1] && c[q] - mean >= 0.6 * (c[best] - mean)) { return q; }
    }
    return best;
}

std::size_t paintAnalysis(Rgba* px, int w, int h, Analysis mode, const DemodFeed& feed, double timebaseMsPerDiv, AnalysisState& st) {
    if (px == nullptr || w < 120 || h < 100 || mode == Analysis::None) { return 0; }
    Canvas cv(px, w, h);
    const float* audio = feed.audio;
    std::size_t na = feed.audioCount;
    const double rate = feed.audioRateHz > 0 ? feed.audioRateHz : 48000.0;
    std::size_t drawn = 0;
    st.peakLagMs = st.peakHz = st.pitchHz = 0;
    st.eyeTraces = 0;
    if (mode == Analysis::Waterfall) {
        st.title = "AUDIO WATERFALL";
        if (st.hw != w || st.hh != h || st.history.size() != static_cast<std::size_t>(w) * h) { st.hw = w; st.hh = h; st.history.assign(static_cast<std::size_t>(w) * h, kBg); st.lines = 0; }
        std::size_t n = 2048;
        while (n > na && n > 64) { n >>= 1; }
        if (audio != nullptr && na >= 64 && n <= na) {
            fftPowerDb(audio + (na - n), n, n / 2, st.db);
            std::memmove(st.history.data() + w, st.history.data(), sizeof(Rgba) * static_cast<std::size_t>(w) * static_cast<std::size_t>(h - 1));
            const auto& pal = foxsdr::slintextras::paletteTable(0);
            for (int x = 0; x < w; ++x) {
                const std::size_t b0 = static_cast<std::size_t>(x) * st.db.size() / static_cast<std::size_t>(w), b1 = std::max(b0 + 1, static_cast<std::size_t>(x + 1) * st.db.size() / static_cast<std::size_t>(w));
                float v = -200.0f;
                for (std::size_t b = b0; b < b1 && b < st.db.size(); ++b) { v = std::max(v, st.db[b]); }
                st.history[static_cast<std::size_t>(x)] = pal[foxsdr::slintextras::paletteIndexOf((v + 100.0f) / 100.0f)];   // -100 dBFS .. 0 dBFS
            }
            ++st.lines;
            ++drawn;
        }
        std::copy(st.history.begin(), st.history.end(), px);
        text(px, w, h, 6, 4, st.title + "  0 - " + f1("%.0f", rate / 2.0) + " HZ");
        return drawn;
    }
    graticule(cv, w, h);
    if (mode == Analysis::Autocorrelation) {
        st.title = "AUTOCORRELATION";
        const std::size_t n = std::min<std::size_t>(na, 2048);
        if (audio != nullptr && n >= 256) {
            autocorrelation(audio + (na - n), n, n / 2, st.r);
            cv.line(0, static_cast<float>(h) / 2.0f, static_cast<float>(w), static_cast<float>(h) / 2.0f, 1.0f, Rgba{kGrid.r, kGrid.g, kGrid.b, 255});
            float px0 = 0, py0 = 0;
            for (std::size_t k = 0; k < st.r.size(); ++k) {
                const float x = static_cast<float>(k) * static_cast<float>(w - 1) / static_cast<float>(st.r.size() - 1);
                const float y = static_cast<float>(h) / 2.0f - st.r[k] * static_cast<float>(h) * 0.42f;
                if (k > 0) { cv.line(px0, py0, x, y, 1.5f, kBeam); ++drawn; }
                px0 = x; py0 = y;
            }
            const std::size_t pk = autocorrelationPeak(st.r);
            if (pk > 0) {
                st.peakLagMs = 1000.0 * static_cast<double>(pk) / rate;
                st.peakHz = rate / static_cast<double>(pk);
                const float x = static_cast<float>(pk) * static_cast<float>(w - 1) / static_cast<float>(st.r.size() - 1);
                cv.line(x, 0, x, static_cast<float>(h), 1.0f, Rgba{0xff, 0xc0, 0x4a, 200});
                text(px, w, h, 6, 16, "PEAK " + f1("%.2f", st.peakLagMs) + " MS  " + f1("%.0f", st.peakHz) + " HZ");
            } else {
                text(px, w, h, 6, 16, "NO REPEAT");
            }
        }
        text(px, w, h, 6, 4, st.title + "  0 - " + f1("%.1f", 1000.0 * static_cast<double>(std::min<std::size_t>(na, 2048) / 2) / rate) + " MS");
        return drawn;
    }
    if (mode == Analysis::Cepstrum) {
        st.title = "CEPSTRUM";
        std::size_t n = 2048;
        while (n > na && n > 64) { n >>= 1; }
        if (audio != nullptr && na >= 64 && n <= na) {
            fftPowerDb(audio + (na - n), n, n / 2, st.db);
            const std::size_t maxQ = std::min<std::size_t>(n / 2 - 1, static_cast<std::size_t>(rate / 50.0));
            cepstrum(st.db, maxQ, st.c);
            float mx = 1e-6f;
            for (std::size_t q = 2; q < st.c.size(); ++q) { mx = std::max(mx, std::fabs(st.c[q])); }
            float px0 = 0, py0 = 0;
            for (std::size_t q = 2; q < st.c.size(); ++q) {
                const float x = static_cast<float>(q - 2) * static_cast<float>(w - 1) / static_cast<float>(st.c.size() - 3);
                const float y = static_cast<float>(h) / 2.0f - st.c[q] / mx * static_cast<float>(h) * 0.42f;
                if (q > 2) { cv.line(px0, py0, x, y, 1.5f, kBeam); ++drawn; }
                px0 = x; py0 = y;
            }
            const std::size_t lo = std::max<std::size_t>(2, static_cast<std::size_t>(rate / 1000.0)), hi = static_cast<std::size_t>(rate / 60.0);
            const std::size_t pk = cepstrumPeak(st.c, lo, hi);
            if (pk > 0) {
                st.pitchHz = rate / static_cast<double>(pk);
                const float x = static_cast<float>(pk - 2) * static_cast<float>(w - 1) / static_cast<float>(st.c.size() - 3);
                cv.line(x, 0, x, static_cast<float>(h), 1.0f, Rgba{0xff, 0xc0, 0x4a, 200});
                text(px, w, h, 6, 16, "PITCH " + f1("%.0f", st.pitchHz) + " HZ");
            } else {
                text(px, w, h, 6, 16, "NO PITCH");
            }
        }
        text(px, w, h, 6, 4, st.title + "  QUEFRENCY");
        return drawn;
    }
    // THE EYE: the I of the baseband (the audio when there is no I/Q) folded on two symbols, one symbol = five divisions of the time base
    st.title = "EYE";
    const float* x = feed.iqI != nullptr && feed.iqCount > 0 ? feed.iqI : audio;
    const std::size_t n = feed.iqI != nullptr && feed.iqCount > 0 ? feed.iqCount : na;
    const double sr = feed.iqI != nullptr && feed.iqCount > 0 && feed.iqRateHz > 0 ? feed.iqRateHz : rate;
    const double spu = 5.0 * timebaseMsPerDiv / 1000.0 * sr;   // samples per symbol
    if (x != nullptr && spu >= 4.0 && spu * 2.0 <= static_cast<double>(n)) {
        float peak = 1e-9f;
        for (std::size_t i = 0; i < n; ++i) { peak = std::max(peak, std::fabs(x[i])); }
        const std::size_t period = static_cast<std::size_t>(2.0 * spu);
        for (std::size_t s0 = 0; s0 + period <= n; s0 += period) {
            float lx = 0, ly = 0;
            for (std::size_t i = 0; i < period; ++i) {
                const float fx = static_cast<float>(i) * static_cast<float>(w - 1) / static_cast<float>(period - 1);
                const float fy = static_cast<float>(h) / 2.0f - x[s0 + i] / peak * static_cast<float>(h) * 0.42f;
                if (i > 0) { cv.line(lx, ly, fx, fy, 1.0f, Rgba{kBeam.r, kBeam.g, kBeam.b, 110}); ++drawn; }
                lx = fx; ly = fy;
            }
            ++st.eyeTraces;
        }
    }
    text(px, w, h, 6, 4, "EYE  2 SYMBOLS = " + f1("%.0f", 10.0 * timebaseMsPerDiv) + " MS");
    if (st.eyeTraces == 0) { text(px, w, h, 6, 16, "NO SIGNAL FOR THIS TIME BASE"); }
    return drawn;
}

}  // namespace foxsdr::slintscope
