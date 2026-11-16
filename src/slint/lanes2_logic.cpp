// lanes2_logic.cpp - see lanes2_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanes2_logic.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>

#include "core/i18n.hpp"
#include "core/rec_naming.hpp"

namespace foxsdr::slints2 {

namespace sll = foxsdr::slintlanel;

namespace {
std::string lower(std::string s) { for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return s; }
std::string fmt(const char* f, double a) { char b[96]; std::snprintf(b, sizeof b, f, a); return b; }
}  // namespace

// ============================================================ FFT size, window, frame rate
std::uint32_t fftSizeAt(int index) { return 1024u << static_cast<unsigned>(std::clamp(index, 0, kFftSizeCount - 1)); }
int fftIndexOf(std::uint32_t size) {
    for (int i = 0; i < kFftSizeCount; ++i) { if (fftSizeAt(i) == size) { return i; } }
    return -1;
}
std::vector<std::string> fftSizeLabels() {
    std::vector<std::string> v;
    for (int i = 0; i < kFftSizeCount; ++i) { v.push_back(std::to_string(fftSizeAt(i))); }
    return v;
}
const char* windowName(int index) {
    static const char* const k[kWindowCount] = {"Rectangular", "Hann", "Hamming", "Blackman-Harris", "Flat top"};
    return k[std::clamp(index, 0, kWindowCount - 1)];
}
bool parseFrameRate(const std::string& text, double& out, std::string& err) {
    const std::string t = lower(sll::trim(text));
    if (t == "auto" || t == "automatic") { out = 0.0; return true; }
    double v = 0;
    if (!sll::parseNumber(t, 0.0, 120.0, v, err)) { return false; }
    if (v != 0.0 && v < 1.0) { err = cascade::i18n::tr("The frame rate is 0 (automatic) or 1 to 120 a second"); return false; }
    out = v;
    return true;
}
std::string frameRateText(double hz) { return hz <= 0.0 ? std::string("0") : sll::numberText(hz); }
std::string spectrumFigure(const SpectrumSetting& s, const std::string& autoWord, const std::string& perSecondWord) {
    return std::to_string(s.size) + " / " + (s.fps <= 0.0 ? autoWord : sll::numberText(s.fps) + " " + perSecondWord);
}

// ============================================================ IF zoom
std::uint32_t zoomSizeAt(int index) { const int i = std::clamp(index, 0, kZoomSizeCount - 1); return i == 0 ? 0u : 128u << static_cast<unsigned>(i); }
int zoomIndexOf(std::uint32_t size) {
    for (int i = 0; i < kZoomSizeCount; ++i) { if (zoomSizeAt(i) == size) { return i; } }
    return -1;
}
std::vector<std::string> zoomSizeLabels(const std::string& offWord) {
    std::vector<std::string> v{offWord};
    for (int i = 1; i < kZoomSizeCount; ++i) { v.push_back(std::to_string(zoomSizeAt(i))); }
    return v;
}
bool parseZoomSpan(const std::string& text, double& out, std::string& err) {
    if (!sll::parseNumber(text, 100.0, 1e7, out, err)) { return false; }
    return true;
}
bool parseZoomFact(const std::string& fact, ZoomFact& out) {
    out = ZoomFact{};
    if (fact.empty()) { return false; }
    if (fact == "off") { return true; }
    unsigned long bins = 0; double binHz = 0, span = 0;
    if (std::sscanf(fact.c_str(), "%lu bins of %lf Hz over %lf Hz", &bins, &binHz, &span) != 3) { return false; }
    out.on = true; out.bins = static_cast<std::uint32_t>(bins); out.binHz = binHz; out.spanHz = span;
    return true;
}

std::vector<std::uint8_t> paintZoom(std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax, double usableFrac) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) { px[i] = 0x0b; px[i + 1] = 0x0d; px[i + 2] = 0x12; px[i + 3] = 255; }
    if (w < 4 || h < 4) { return px; }
    const auto set = [&](std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x >= w || y >= h) { return; }
        std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // the graticule: ten columns and five rows
    for (std::uint32_t k = 1; k < 10; ++k) { for (std::uint32_t y = 0; y < h; y += 2) { set(k * w / 10, y, 0x25, 0x2a, 0x34); } }
    for (std::uint32_t k = 1; k < 5; ++k) { for (std::uint32_t x = 0; x < w; x += 2) { set(x, k * h / 5, 0x25, 0x2a, 0x34); } }
    set(w / 2, 0, 0x60, 0x60, 0x40);
    for (std::uint32_t y = 0; y < h; ++y) { if ((y & 3u) == 0) { set(w / 2, y, 0x50, 0x50, 0x38); } }
    if (bins == nullptr || n == 0 || !(dbMax > dbMin)) { return px; }
    const double usable = std::clamp(usableFrac, 0.1, 1.0);
    const double edge = (1.0 - usable) / 2.0;
    for (std::uint32_t x = 0; x < w; ++x) {
        const std::size_t b0 = static_cast<std::size_t>(static_cast<double>(x) * static_cast<double>(n) / w);
        const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(static_cast<double>(x + 1) * static_cast<double>(n) / w));
        float m = -std::numeric_limits<float>::infinity();
        for (std::size_t b = b0; b < std::min(b1, n); ++b) { if (std::isfinite(bins[b])) { m = std::max(m, bins[b]); } }
        if (!std::isfinite(m)) { continue; }
        const double t = std::clamp((static_cast<double>(m) - dbMin) / (dbMax - dbMin), 0.0, 1.0);
        const std::uint32_t top = h - 1u - static_cast<std::uint32_t>(t * static_cast<double>(h - 1u) + 0.5);
        const double fx = (static_cast<double>(x) + 0.5) / w;
        const bool clean = fx >= edge && fx <= 1.0 - edge;
        const std::uint8_t k = clean ? 255 : 110;
        for (std::uint32_t y = top; y < h; ++y) {   // a faint fill under the line
            set(x, y, static_cast<std::uint8_t>(0x0b + (0x1c * k) / 255), static_cast<std::uint8_t>(0x0d + (0x3a * k) / 255), static_cast<std::uint8_t>(0x12 + (0x50 * k) / 255));
        }
        set(x, top, static_cast<std::uint8_t>(0x7a * k / 255), static_cast<std::uint8_t>(0xd7 * k / 255), static_cast<std::uint8_t>(0xff * k / 255));
    }
    return px;
}

std::uint32_t paintedPeakColumn(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h) {
    std::uint32_t bestX = 0, bestTop = h;
    for (std::uint32_t x = 0; x < w; ++x) {
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4u];
            if (p[0] == 0x7a && p[1] == 0xd7 && p[2] == 0xff) { if (y < bestTop) { bestTop = y; bestX = x; } break; }
        }
    }
    return bestX;
}

// ============================================================ autosave
int autosaveMinutes(int index) {
    static const int k[kAutosaveChoices] = {1, 5, 15, 30, 60};
    return k[std::clamp(index, 0, kAutosaveChoices - 1)];
}
int autosaveIndexOf(int minutes) {
    for (int i = 0; i < kAutosaveChoices; ++i) { if (autosaveMinutes(i) == minutes) { return i; } }
    return -1;
}
bool autosaveDue(std::int64_t lastSavedMs, std::int64_t nowMs, int minutes) {
    if (minutes <= 0) { return false; }
    if (lastSavedMs <= 0) { return true; }
    return nowMs - lastSavedMs >= static_cast<std::int64_t>(minutes) * 60000;
}
std::string cleanFolder(const std::string& typed) {
    std::string t = sll::trim(typed);
    while (t.size() >= 2 && t.front() == '"' && t.back() == '"') { t = sll::trim(t.substr(1, t.size() - 2)); }
    while (!t.empty() && (t.back() == '/' || t.back() == '\\')) { if (t.size() == 3 && t[1] == ':') { break; } t.pop_back(); }
    if (t.empty()) { return std::string(); }
    const bool drive = t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) && t[1] == ':' && (t[2] == '/' || t[2] == '\\');
    const bool unc = t.size() >= 3 && (t[0] == '\\' || t[0] == '/') && t[0] == t[1];
    const bool posix = t[0] == '/' && t.size() > 1 && t[1] != '/';
    if (!drive && !unc && !posix) { return std::string(); }
    return t;
}
std::string autosavePath(const std::string& folder, const std::string& recorderTemplate, const AutosaveFields& f, std::uint64_t counter) {
    namespace rn = cascade::core::recname;
    if (folder.empty()) { return std::string(); }
    rn::Fields fl;
    fl.unixMs = f.unixMs; fl.utcOffsetMinutes = f.utcOffsetMinutes; fl.frequencyHz = f.frequencyHz; fl.mode = f.mode; fl.radio = std::to_string(f.radio); fl.counter = counter;
    rn::Expanded e = recorderTemplate.empty() ? rn::Expanded{} : rn::expand(recorderTemplate, fl, true);
    if (!e.ok) { e = rn::expand("waterfall_{datetime}", fl, true); }
    if (!e.ok) { return std::string(); }
    std::string err;
    const std::string full = rn::resolvePath(folder, e.relPath, "png", &err);
    if (full.empty()) { return std::string(); }
    // the extension is added by the writer: give back the path without it
    return full.substr(0, full.size() - 4);
}

// ============================================================ the power and SNR log
bool parseLogInterval(const std::string& text, double& out, std::string& err) { return sll::parseNumber(text, 0.1, 3600.0, out, err); }

PowerReading readPower(const float* bins, std::size_t n, std::size_t p0, std::size_t p1) {
    PowerReading r;
    if (bins == nullptr || n == 0 || p1 <= p0 || p0 >= n) { return r; }
    p1 = std::min(p1, n);
    double sum = 0.0; float peak = -std::numeric_limits<float>::infinity();
    for (std::size_t i = p0; i < p1; ++i) { if (std::isfinite(bins[i])) { sum += std::pow(10.0, static_cast<double>(bins[i]) / 10.0); peak = std::max(peak, bins[i]); } }
    if (!(sum > 0.0) || !std::isfinite(peak)) { return r; }
    std::vector<float> outside;
    outside.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { if ((i < p0 || i >= p1) && std::isfinite(bins[i])) { outside.push_back(bins[i]); } }
    if (outside.empty()) { for (std::size_t i = 0; i < n; ++i) { if (std::isfinite(bins[i])) { outside.push_back(bins[i]); } } }
    if (outside.empty()) { return r; }
    std::nth_element(outside.begin(), outside.begin() + static_cast<std::ptrdiff_t>(outside.size() / 2), outside.end());
    r.valid = true;
    r.signalDbfs = 10.0 * std::log10(sum);
    r.noiseDbfs = static_cast<double>(outside[outside.size() / 2]);
    r.snrDb = static_cast<double>(peak) - r.noiseDbfs;
    return r;
}

// ============================================================ band stacking
namespace {
// Amateur bands (the widest limits of the three IARU regions: the registers hold where the person was, not an allocation), then the shortwave broadcast bands and the common VHF ones.
const BandInfo kBands[] = {
    {"160 m", 1.8e6, 2.0e6, "LSB", 2700, 1.84e6},       {"80 m", 3.5e6, 4.0e6, "LSB", 2700, 3.7e6},         {"60 m", 5.25e6, 5.45e6, "USB", 2700, 5.3665e6},
    {"40 m", 7.0e6, 7.3e6, "LSB", 2700, 7.1e6},         {"30 m", 10.1e6, 10.15e6, "CW", 500, 10.12e6},      {"20 m", 14.0e6, 14.35e6, "USB", 2700, 14.2e6},
    {"17 m", 18.068e6, 18.168e6, "USB", 2700, 18.12e6}, {"15 m", 21.0e6, 21.45e6, "USB", 2700, 21.25e6},    {"12 m", 24.89e6, 24.99e6, "USB", 2700, 24.94e6},
    {"10 m", 28.0e6, 29.7e6, "USB", 2700, 28.4e6},      {"6 m", 50.0e6, 54.0e6, "USB", 2700, 50.2e6},       {"2 m", 144.0e6, 148.0e6, "NFM", 12500, 145.5e6},
    {"70 cm", 430.0e6, 440.0e6, "NFM", 12500, 433.5e6}, {"49 m", 5.9e6, 6.2e6, "AM", 10000, 6.1e6},         {"31 m", 9.4e6, 9.9e6, "AM", 10000, 9.6e6},
    {"25 m", 11.6e6, 12.1e6, "AM", 10000, 11.8e6},      {"19 m", 15.1e6, 15.8e6, "AM", 10000, 15.4e6},      {"16 m", 17.48e6, 17.9e6, "AM", 10000, 17.7e6},
    {"FM broadcast", 87.5e6, 108.0e6, "WFM", 150000, 98.0e6}, {"Air band", 118.0e6, 137.0e6, "AM", 8330, 124.0e6},
};
}  // namespace
int bandCount() { return static_cast<int>(sizeof(kBands) / sizeof(kBands[0])); }
const BandInfo& bandAt(int index) { return kBands[std::clamp(index, 0, bandCount() - 1)]; }
std::string registerText(double hz, const std::string& mode, double bandwidthHz) {
    std::string s = fmt("%.6f", hz / 1e6) + " MHz";
    if (!mode.empty()) { s += " " + mode; }
    if (bandwidthHz > 0.0) { s += bandwidthHz >= 1000.0 ? " " + fmt("%.1f", bandwidthHz / 1000.0) + " kHz" : " " + fmt("%.0f", bandwidthHz) + " Hz"; }
    return s;
}

// ============================================================ VFO A / B
const char* vfoLetter(std::uint32_t active) { return active == 0 ? "A" : "B"; }
std::string vfoText(double hz, const std::string& mode, double bandwidthHz) {
    std::string s = fmt("%.6f", hz / 1e6) + " MHz  " + mode;
    if (bandwidthHz > 0.0) { s += bandwidthHz >= 1000.0 ? "  " + fmt("%.1f", bandwidthHz / 1000.0) + " kHz" : "  " + fmt("%.0f", bandwidthHz) + " Hz"; }
    return s;
}

}  // namespace foxsdr::slints2
