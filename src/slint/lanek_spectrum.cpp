// lanek_spectrum.cpp - see lanek_spectrum.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanek_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace foxsdr::slintlanek {

namespace dsp = cascade::dsp;
using foxsdr::slinthost::Rgba;

void detectorColumns(const float* db, std::size_t n, std::size_t points, dsp::RbwDetector det, float* out, float floorDb) {
    if (out == nullptr || points == 0) { return; }
    if (db == nullptr || n == 0) { for (std::size_t c = 0; c < points; ++c) { out[c] = floorDb; } return; }
    const auto val = [&](std::size_t i) { const float v = db[i]; return std::isfinite(v) ? v : floorDb; };
    for (std::size_t c = 0; c < points; ++c) {
        std::size_t a = c * n / points;
        std::size_t b = (c + 1) * n / points;
        if (b <= a) { b = a + 1; }
        if (b > n) { b = n; a = std::min(a, n - 1); }
        const std::size_t count = b - a;
        double r = 0.0;
        switch (det) {
            case dsp::RbwDetector::Peak: { float m = val(a); for (std::size_t i = a + 1; i < b; ++i) { m = std::max(m, val(i)); } r = m; break; }
            case dsp::RbwDetector::NegPeak: { float m = val(a); for (std::size_t i = a + 1; i < b; ++i) { m = std::min(m, val(i)); } r = m; break; }
            case dsp::RbwDetector::Rms: {   // 10 log10 of the mean POWER
                double s = 0.0; for (std::size_t i = a; i < b; ++i) { s += std::pow(10.0, val(i) / 10.0); }
                r = 10.0 * std::log10(std::max(s / static_cast<double>(count), 1e-30)); break;
            }
            case dsp::RbwDetector::AvgVolt: {   // (mean of sqrt(power))^2: the voltage ("linear") average
                double s = 0.0; for (std::size_t i = a; i < b; ++i) { s += std::pow(10.0, val(i) / 20.0); }
                const double m = s / static_cast<double>(count);
                r = 20.0 * std::log10(std::max(m, 1e-15)); break;
            }
            case dsp::RbwDetector::AvgLog: {   // mean of the dB values
                double s = 0.0; for (std::size_t i = a; i < b; ++i) { s += val(i); }
                r = s / static_cast<double>(count); break;
            }
            case dsp::RbwDetector::Sample: r = val(std::min(n - 1, (a + b) / 2)); break;
        }
        out[c] = static_cast<float>(r);
    }
}

double nativeRbwHz(double sampleRateHz, std::size_t bins) {
    if (!(sampleRateHz > 0.0) || bins < 2) { return 0.0; }
    return dsp::rbwEnbwHz(dsp::RbwWindow::BlackmanHarris, sampleRateHz, bins);
}

std::string rbwText(double hz) {
    char b[48] = {};
    if (!(hz > 0.0) || !std::isfinite(hz)) { return "n/a"; }
    if (hz >= 1e6) { std::snprintf(b, sizeof(b), "%.2f MHz", hz / 1e6); }
    else if (hz >= 1e3) { std::snprintf(b, sizeof(b), "%.2f kHz", hz / 1e3); }
    else { std::snprintf(b, sizeof(b), "%.0f Hz", hz); }
    return b;
}

bool maskLimitAt(const cascade::core::Mask& m, double hz, bool upper, double& db) {
    bool have = false;
    double best = 0.0;
    for (const auto& s : m.segments) {
        if ((s.kind == cascade::core::MaskKind::Upper) != upper) { continue; }
        if (hz < s.f1 || hz > s.f2 || !(s.f2 > s.f1)) { continue; }
        const double l = s.l1 + (s.l2 - s.l1) * (hz - s.f1) / (s.f2 - s.f1);
        if (!have) { best = l; have = true; }
        else { best = upper ? std::min(best, l) : std::max(best, l); }
    }
    if (have) { db = best; }
    return have;
}

namespace {
void plot(Rgba* px, std::uint32_t w, std::uint32_t h, long x, long y, Rgba c) {
    if (x < 0 || y < 0 || x >= static_cast<long>(w) || y >= static_cast<long>(h)) { return; }
    px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = c;
}
}  // namespace

void paintMask(Rgba* px, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, double dbMin, double dbMax, const cascade::core::Mask& m) {
    if (px == nullptr || w < 2 || h < 2 || !(hiHz > loHz) || !(dbMax > dbMin)) { return; }
    const Rgba red{255, 80, 80, 255}, cyan{80, 220, 255, 255};
    for (std::uint32_t x = 0; x < w; ++x) {
        const double hz = loHz + (static_cast<double>(x) + 0.5) / static_cast<double>(w) * (hiHz - loHz);
        for (int pass = 0; pass < 2; ++pass) {
            double db = 0.0;
            if (!maskLimitAt(m, hz, pass == 0, db)) { continue; }
            const double f = (db - dbMin) / (dbMax - dbMin);
            const long y = static_cast<long>(std::lround((1.0 - std::clamp(f, 0.0, 1.0)) * static_cast<double>(h - 1)));
            plot(px, w, h, static_cast<long>(x), y, pass == 0 ? red : cyan);
            plot(px, w, h, static_cast<long>(x), y + 1, pass == 0 ? red : cyan);
        }
    }
}

bool parseMaskText(const std::string& text, cascade::core::Mask& out, std::string& error) {
    cascade::core::Mask m;
    const auto r = cascade::core::parseMask(text, m);
    if (r.error != cascade::core::MaskError::None) { error = cascade::core::maskErrorText(r.error); return false; }
    out = std::move(m);
    return true;
}

void paintCursors(Rgba* px, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, const std::array<double, 8>& freqHz, const std::array<bool, 8>& shown) {
    if (px == nullptr || w < 16 || h < 16 || !(hiHz > loHz)) { return; }
    static const Rgba kColours[8] = {{255, 214, 10, 255}, {10, 255, 180, 255}, {255, 120, 220, 255}, {120, 180, 255, 255}, {255, 160, 60, 255}, {190, 255, 80, 255}, {200, 140, 255, 255}, {255, 255, 255, 255}};
    for (std::size_t i = 0; i < 8; ++i) {
        if (!shown[i] || !std::isfinite(freqHz[i]) || freqHz[i] < loHz || freqHz[i] > hiHz) { continue; }
        const long x = static_cast<long>(std::lround((freqHz[i] - loHz) / (hiHz - loHz) * static_cast<double>(w - 1)));
        for (std::uint32_t y = 0; y < h; y += 2) { plot(px, w, h, x, static_cast<long>(y), kColours[i]); }   // dotted
        for (long dy = 0; dy < 7; ++dy) { for (long dx = -3; dx <= 3; ++dx) { plot(px, w, h, x + dx, dy, kColours[i]); } }   // the tab
        // the number inside the tab: i + 1 as 1 to 8 small ticks in black (a 7 x 7 box cannot hold a glyph; the sheet lists the numbers)
        for (std::size_t k = 0; k <= i && k < 7; ++k) { plot(px, w, h, x - 3 + static_cast<long>(k), 3, Rgba{0, 0, 0, 255}); }
    }
}

double cursorLevel(const float* db, std::size_t n, double centreHz, double spanHz, double freqHz, double searchHz) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (db == nullptr || n < 2 || !(spanHz > 0.0) || !std::isfinite(freqHz)) { return nan; }
    const double binHz = spanHz / static_cast<double>(n);
    const double lo = centreHz - spanHz / 2.0;
    const long c = static_cast<long>(std::floor((freqHz - lo) / binHz));
    if (c < 0 || c >= static_cast<long>(n)) { return nan; }
    const long reach = std::max(0L, static_cast<long>(std::lround(std::max(0.0, searchHz) / binHz)));
    const long a = std::max(0L, c - reach), b = std::min(static_cast<long>(n) - 1, c + reach);
    double best = -std::numeric_limits<double>::infinity();
    for (long i = a; i <= b; ++i) { if (std::isfinite(db[i])) { best = std::max(best, static_cast<double>(db[i])); } }
    return std::isfinite(best) ? best : nan;
}

}  // namespace foxsdr::slintlanek
