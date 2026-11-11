// The Slint host's pure logic - see host_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_logic.hpp"

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace foxsdr::slinthost {
namespace {

const char* const kDot = " \xC2\xB7 ";   // the IA's middle dot between clauses

// Into a std::string, never a fixed buffer: a format that comes back from tr() may be longer than the English
// (tests/test_i18n.cpp scans for snprintf into a sizeof-bounded buffer and refuses it).
std::string fmt(const char* f, double v) {
    std::string s;
    cascade::core::formatUtf8(s, f, v);
    return s;
}

std::string joinDot(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) { continue; }
        if (!out.empty()) { out += kDot; }
        out += p;
    }
    return out;
}

double niceStep(double raw) {
    if (!(raw > 0.0)) { return 0.0; }
    const double base = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (m * base >= raw * (1.0 - 1e-9)) { return m * base; }
    }
    return 10.0 * base;
}

float columnMax(const float* bins, std::size_t n, std::uint32_t x, std::uint32_t width) {
    const std::size_t b0 = static_cast<std::size_t>(x) * n / width;
    const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(x + 1) * n / width);
    float v = -300.0f;
    for (std::size_t b = b0; b < b1 && b < n; ++b) { v = std::max(v, bins[b]); }
    return v;
}

}  // namespace

// ---- digits ----
std::array<int, 10> digitsOf(double hz) {
    std::array<int, 10> d{};
    long long n = std::llround(std::clamp(hz, 0.0, 9'999'999'999.0));
    n = std::min(n, 9'999'999'999LL);
    for (int i = 9; i >= 0; --i) { d[static_cast<std::size_t>(i)] = static_cast<int>(n % 10); n /= 10; }
    return d;
}
int leadingZeros(const std::array<int, 10>& d) {
    int n = 0;
    for (int v : d) { if (v != 0) { break; } ++n; }
    return std::min(n, 9);
}
double stepOfDigit(int index) {
    double s = 1.0;
    for (int i = 9; i > index; --i) { s *= 10.0; }
    return s;
}
const char* placeName(int index) {
    static const char* const names[10] = {"GHz", "100 MHz", "10 MHz", "1 MHz", "100 kHz", "10 kHz", "1 kHz", "100 Hz", "10 Hz", "1 Hz"};
    return (index >= 0 && index < 10) ? names[index] : "";
}

// ---- modes ----
const char* modeName(std::uint32_t demod) {
    switch (demod) {
        case FOXAPI_DEMOD_NFM: return "NFM"; case FOXAPI_DEMOD_WFM: return "WFM"; case FOXAPI_DEMOD_AM: return "AM";
        case FOXAPI_DEMOD_DSB: return "DSB"; case FOXAPI_DEMOD_USB: return "USB"; case FOXAPI_DEMOD_CW: return "CW";
        case FOXAPI_DEMOD_LSB: return "LSB"; case FOXAPI_DEMOD_RAW: return "RAW"; default: return "?";
    }
}
int modeIndex(std::uint32_t demod) {
    return (demod >= FOXAPI_DEMOD_NFM && demod <= FOXAPI_DEMOD_RAW) ? static_cast<int>(demod - FOXAPI_DEMOD_NFM) : -1;
}
std::uint32_t demodOfIndex(int index) { return static_cast<std::uint32_t>(FOXAPI_DEMOD_NFM + std::clamp(index, 0, 7)); }
double modeStepHz(std::uint32_t demod) {
    switch (demod) {
        case FOXAPI_DEMOD_NFM: return 12500.0;
        case FOXAPI_DEMOD_WFM: return 100000.0;
        case FOXAPI_DEMOD_AM: return 9000.0;
        default: return 1000.0;
    }
}

// ---- the four lists a radio has of its own ----
bool isPerRadioList(std::uint32_t list) {
    return list == FOXAPI_LIST_GAINS || list == FOXAPI_LIST_ANTENNAS || list == FOXAPI_LIST_SAMPLE_RATES || list == FOXAPI_LIST_DECODERS;
}

std::int32_t readListFor(const FoxEngineApi* api, FoxSession* session, std::uint32_t radio, std::uint32_t list, std::uint32_t index, FoxListItem* out) {
    if (api == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    if (radio >= 1u && isPerRadioList(list) && FOXAPI_HAS(api, read_radio_list)) { return api->read_radio_list(session, radio, list, index, out); }
    return api->read_list(session, list, index, out);
}

// ---- readouts ----
std::string fmtMHz(double hz) { return fmt("%.6f MHz", hz / 1e6); }
std::string fmtRate(double hz) { return hz > 0.0 ? fmt("%.3f MS/s", hz / 1e6) : std::string("--"); }
std::string fmtKHz(double hz) { return fmt("%.1f kHz", hz / 1e3); }
std::string spanText(double hz) {
    if (hz >= 1e9) { return cascade::core::formatText(cascade::i18n::tr("SPAN %s"), fmt("%.3f GHz", hz / 1e9).c_str()); }
    if (hz >= 1e6) { return cascade::core::formatText(cascade::i18n::tr("SPAN %s"), fmt("%.3f MHz", hz / 1e6).c_str()); }
    if (hz >= 1e3) { return cascade::core::formatText(cascade::i18n::tr("SPAN %s"), fmt("%.1f kHz", hz / 1e3).c_str()); }
    return cascade::core::formatText(cascade::i18n::tr("SPAN %s"), fmt("%.0f Hz", hz).c_str());
}
std::string cursorText(double hz, double db) { return fmt("%.3f MHz", hz / 1e6) + kDot + fmt("%.1f dB", db); }
std::string axisUnit(double) { return "MHz"; }

bool parseFrequencyHz(const std::string& text, double& hz) {
    std::string digits;
    std::string unit;
    bool seenDot = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') { digits += ch; }
        else if (ch == '.' && !seenDot) { digits += ch; seenDot = true; }
        else if (ch == ',' && !seenDot) { digits += '.'; seenDot = true; }   // a decimal comma
        else if (ch == ' ' || ch == '\t' || ch == '_' || ch == '\'') { continue; }
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) { unit += static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        else { return false; }
    }
    if (digits.empty() || digits == ".") { return false; }
    const double value = std::strtod(digits.c_str(), nullptr);
    if (!(value >= 0.0)) { return false; }
    double scale = 0.0;
    if (unit.empty()) { scale = value < 100000.0 ? 1e6 : 1.0; }
    else if (unit == "ghz") { scale = 1e9; }
    else if (unit == "mhz" || unit == "m") { scale = 1e6; }
    else if (unit == "khz" || unit == "k") { scale = 1e3; }
    else if (unit == "hz") { scale = 1.0; }
    else { return false; }
    hz = value * scale;
    return hz > 0.0 && hz < 1e11;
}

double snapToStep(double hz, double stepHz, bool shift) {
    if (shift || !(stepHz > 0.0)) { return hz; }
    return std::round(hz / stepHz) * stepHz;
}

ViewWindow zoomAbout(ViewWindow v, double fullCentre, double fullSpan, double factor, double aboutFrac, double minSpan) {
    if (!(fullSpan > 0.0)) { return v; }
    if (!(v.spanHz > 0.0)) { v = {fullCentre, fullSpan}; }
    const double fixedHz = hzAt(std::clamp(aboutFrac, 0.0, 1.0), v.centreHz, v.spanHz);
    double span = std::clamp(v.spanHz / std::max(1e-9, factor), std::max(1.0, minSpan), fullSpan);
    double centre = fixedHz - (std::clamp(aboutFrac, 0.0, 1.0) - 0.5) * span;
    const double lo = fullCentre - fullSpan / 2.0 + span / 2.0, hi = fullCentre + fullSpan / 2.0 - span / 2.0;
    centre = std::clamp(centre, std::min(lo, hi), std::max(lo, hi));
    return {centre, span};
}

ViewWindow panBy(ViewWindow v, double fullCentre, double fullSpan, double dFrac) {
    if (!(fullSpan > 0.0) || !(v.spanHz > 0.0)) { return v; }
    double centre = v.centreHz - dFrac * v.spanHz;
    const double lo = fullCentre - fullSpan / 2.0 + v.spanHz / 2.0, hi = fullCentre + fullSpan / 2.0 - v.spanHz / 2.0;
    centre = std::clamp(centre, std::min(lo, hi), std::max(lo, hi));
    return {centre, v.spanHz};
}

// ---- dials ----
double squelchNorm(double db) { return squelchAuto(db) ? 0.0 : std::clamp((db + 120.0) / 120.0, 0.0, 1.0); }
double squelchDbFromNorm(double norm) { return -120.0 + 120.0 * std::clamp(norm, 0.0, 1.0); }
bool squelchAuto(double db) { return db <= -199.0; }
double levelNorm(double db) { return std::clamp((db + 60.0) / 60.0, 0.0, 1.0); }
std::string levelText(double db, bool haveAudio) {
    if (!haveAudio) { return "--"; }
    if (db <= -60.0) { return "-60 dB"; }
    return fmt("%.1f dB", db);
}
std::string gainText(bool haveStage, bool agcOn, double value, const std::string& unit) {
    if (!haveStage) { return "N/A"; }
    if (agcOn) { return "AUTO"; }
    return fmt("%.0f ", value) + unit;
}

// ---- axes ----
std::vector<Tick> frequencyTicks(double centreHz, double spanHz, double widthPx, double targetPx) {
    std::vector<Tick> out;
    if (!(spanHz > 0.0) || !(widthPx > 0.0) || !(targetPx > 0.0)) { return out; }
    const double step = niceStep(spanHz * targetPx / widthPx);
    if (!(step > 0.0)) { return out; }
    const double stepMHz = step / 1e6;
    const int decimals = std::max(0, static_cast<int>(std::ceil(-std::log10(stepMHz) - 1e-9)));
    const double left = centreHz - spanHz / 2.0;
    double hz = std::ceil(left / step - 1e-9) * step;
    char f[16];
    std::snprintf(f, sizeof(f), "%%.%df", decimals);
    for (int guard = 0; guard < 64 && hz <= left + spanHz + step * 1e-9; ++guard, hz += step) {
        out.push_back({(hz - left) / spanHz, fmt(f, hz / 1e6)});
    }
    return out;
}

std::vector<Tick> dbTicks(double dbMin, double dbMax, double heightPx) {
    std::vector<Tick> out;
    if (!(dbMax > dbMin)) { return out; }
    const double range = dbMax - dbMin;
    const double pitch = heightPx * 10.0 / range;
    int labelEvery = pitch < 40.0 ? 20 : 10;
    // A SHORT PLOT (a pane of four on a 1280 x 720 window has about 60 px of trace): two labels are never closer than 14 px, so the labels thin out to every 50 or 100 dB instead of running into
    // one smear (found by the S6 sweep screenshots; before, the rule stopped at 20 dB)
    for (const int every : {50, 100}) { if (heightPx * labelEvery / range < 14.0 && labelEvery < every) { labelEvery = every; } }
    const int top = static_cast<int>(std::floor(dbMax / 10.0)) * 10;
    const int bottom = static_cast<int>(std::ceil(dbMin / 10.0)) * 10;
    for (int db = top; db >= bottom; db -= 10) {
        const bool labelled = (db % labelEvery) == 0;
        out.push_back({(dbMax - db) / range, labelled ? std::to_string(db) : std::string()});
        if (out.size() >= 64) { break; }
    }
    return out;
}

double fracOf(double hz, double centreHz, double spanHz) { return spanHz > 0.0 ? (hz - (centreHz - spanHz / 2.0)) / spanHz : 0.5; }
double hzAt(double frac, double centreHz, double spanHz) { return centreHz - spanHz / 2.0 + frac * spanHz; }

std::vector<Tick> ageTicks(double historySeconds, double heightPx, double minPitchPx) {
    std::vector<Tick> out;
    out.push_back({0.0, cascade::i18n::tr("now")});
    if (!(historySeconds > 0.0) || !(heightPx > 0.0)) { return out; }
    const double minStep = minPitchPx * historySeconds / heightPx;   // the step whose labels are minPitchPx apart
    double step = 0.0;
    for (double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0}) {
        if (s >= minStep) { step = s; break; }
    }
    if (step == 0.0) { step = 3600.0 * std::ceil(minStep / 3600.0); }
    for (double t = step; t <= historySeconds + 1e-9 && out.size() < 64; t += step) {
        const std::string label = t < 60.0 ? fmt("%.0f s", t) : fmt("%.0f min", t / 60.0);
        out.push_back({t / historySeconds, label});
    }
    return out;
}

// ---- frames ----
bool isNewFrame(std::uint64_t seq, std::uint64_t lastSeq) { return seq > lastSeq; }

// ---- painters ----
Rgba heat(float t) {
    struct Stop { float at; std::uint8_t r, g, b; };
    static const std::array<Stop, 5> stops{{{0.00f, 0x0a, 0x14, 0x30}, {0.30f, 0x1b, 0x3a, 0x6a}, {0.55f, 0x2e, 0x86, 0xc1},
                                            {0.80f, 0xf5, 0xa5, 0x24}, {1.00f, 0xff, 0xff, 0xff}}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].at) {
            const float f = (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
            const auto mix = [f](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(a + (b - a) * f)); };
            return {mix(stops[i - 1].r, stops[i].r), mix(stops[i - 1].g, stops[i].g), mix(stops[i - 1].b, stops[i].b), 255};
        }
    }
    return {255, 255, 255, 255};
}

void resetTrace(TraceState& s) {
    s.peak.clear();
    s.average.clear();
    s.density.clear();
    s.minHold.clear();
}

namespace {

// THE PERSISTENCE (phosphor) PICTURE: the trace of the last seconds as a density, brighter where the trace spent more of them. One float a cell, decayed and added to every frame, mapped through a 256-step
// ramp (the strength key's heat() with an alpha that rises with the density) so the picture sits over the spectrum's background like the plain trace does. Cells under 2 % are not drawn.
void paintPersistence(Rgba* px, std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax, TraceState& st) {
    const std::size_t cells = static_cast<std::size_t>(w) * h;
    if (st.density.size() != cells) { st.density.assign(cells, 0.0f); }
    const float decay = std::clamp(st.decay, 0.5f, 0.999f);
    float* d = st.density.data();
    for (std::size_t i = 0; i < cells; ++i) { d[i] *= decay; }
    const double range = std::max(1.0, dbMax - dbMin);
    int last = -1;
    for (std::uint32_t x = 0; x < w; ++x) {
        const float v = columnMax(bins, n, x, w);
        const int y = static_cast<int>((1.0 - std::clamp((v - dbMin) / range, 0.0, 1.0)) * (h - 1));
        const int y0 = last < 0 ? y : std::min(last, y), y1 = last < 0 ? y : std::max(last, y);
        for (int yy = y0; yy <= y1; ++yy) { d[static_cast<std::size_t>(yy) * w + x] += 1.0f; }
        last = y;
    }
    static const std::array<Rgba, 256> ramp = [] {
        std::array<Rgba, 256> r{};
        for (std::size_t i = 0; i < r.size(); ++i) {
            const float t = static_cast<float>(i) / 255.0f;
            const Rgba c = heat(std::min(1.0f, 0.25f + 0.75f * t));
            r[i] = Rgba{c.r, c.g, c.b, static_cast<std::uint8_t>(std::lround(255.0 * std::min(1.0, 0.18 + 0.82 * std::sqrt(static_cast<double>(t)))))};
        }
        return r;
    }();
    const float scale = (1.0f - decay) * 255.0f;
    for (std::size_t i = 0; i < cells; ++i) {
        const float t = d[i] * scale;
        px[i] = t < 5.1f ? Rgba{0, 0, 0, 0} : ramp[static_cast<std::size_t>(std::min(255.0f, t))];
    }
}

// THE REFERENCE TRACE (lane B, S22): the saved trace as a thin violet line, one level per column, drawn behind the live trace (NaN columns are gaps).
void paintReference(Rgba* px, std::uint32_t w, std::uint32_t h, const std::vector<float>& ref, double dbMin, double dbMax) {
    if (ref.size() != w) { return; }
    const double range = std::max(1.0, dbMax - dbMin);
    int last = -1;
    for (std::uint32_t x = 0; x < w; ++x) {
        const float v = ref[x];
        if (std::isnan(v)) { last = -1; continue; }
        const int y = static_cast<int>((1.0 - std::clamp((v - dbMin) / range, 0.0, 1.0)) * (h - 1));
        const int y0 = last < 0 ? y : std::min(last, y), y1 = last < 0 ? y : std::max(last, y);
        for (int yy = y0; yy <= y1; ++yy) { px[static_cast<std::size_t>(yy) * w + x] = Rgba{0xc4, 0x8b, 0xff, 230}; }
        last = y;
    }
}

}  // namespace

void paintTrace(Rgba* px, std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax,
                TraceState& st, TraceMode mode, Rgba live, Rgba held, Rgba average) {
    if (px == nullptr || w == 0 || h == 0) { return; }
    std::fill(px, px + static_cast<std::size_t>(w) * h, Rgba{0, 0, 0, 0});
    if (mode == TraceMode::Persistence) {
        if (bins == nullptr || n == 0) { return; }
        st.peak.clear();
        st.average.clear();
        paintPersistence(px, w, h, bins, n, dbMin, dbMax, st);
        if (st.showReference) { paintReference(px, w, h, st.reference, dbMin, dbMax); }
        return;
    }
    st.density.clear();
    if (bins == nullptr || n == 0) { return; }
    const double range = std::max(1.0, dbMax - dbMin);
    const auto yOf = [&](float db) { return static_cast<int>((1.0 - std::clamp((db - dbMin) / range, 0.0, 1.0)) * (h - 1)); };
    const auto put = [&](int x, int y, Rgba c) {
        if (x >= 0 && y >= 0 && x < static_cast<int>(w) && y < static_cast<int>(h)) { px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = c; }
    };
    const auto vline = [&](int x, int y0, int y1, Rgba c) { for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) { put(x, y, c); } };

    if (st.showReference) { paintReference(px, w, h, st.reference, dbMin, dbMax); }
    if (mode == TraceMode::MinHold && st.minHold.size() != w) { st.minHold.assign(w, 300.0f); }
    if (mode != TraceMode::MinHold) { st.minHold.clear(); }
    if (mode == TraceMode::PeakHold && st.peak.size() != w) { st.peak.assign(w, -300.0f); }
    if (mode == TraceMode::Average && st.average.size() != w) { st.average.assign(w, -300.0f); }
    if (mode != TraceMode::PeakHold) { st.peak.clear(); }
    if (mode != TraceMode::Average) { st.average.clear(); }

    // the live trace, joined column to column
    int last = -1;
    std::vector<int> heldY;
    if (mode != TraceMode::Normal) { heldY.assign(w, -1); }
    for (std::uint32_t x = 0; x < w; ++x) {
        const float v = columnMax(bins, n, x, w);
        const int y = yOf(v);
        if (st.shape == 1) {   // FILL (S6): the area under the trace, the trace's colour at about a quarter
            const Rgba f{live.r, live.g, live.b, 72};
            for (int yy = y + 1; yy < static_cast<int>(h); ++yy) { put(static_cast<int>(x), yy, f); }
        }
        if (st.shape == 2) {   // BARS (S6): a histogram of 4-column groups (3 wide, 1 gap), each as high as its strongest column
            const std::uint32_t g0 = x - (x % 4);
            float gv = v;
            for (std::uint32_t k = g0; k < std::min<std::uint32_t>(w, g0 + 4); ++k) { gv = std::max(gv, columnMax(bins, n, k, w)); }
            if (x % 4 != 3) {
                const int yb = yOf(gv);
                const Rgba f{live.r, live.g, live.b, 150};
                for (int yy = yb + 1; yy < static_cast<int>(h); ++yy) { put(static_cast<int>(x), yy, f); }
                put(static_cast<int>(x), yb, live);
            }
        } else if (last >= 0) { vline(static_cast<int>(x), last, y, live); } else { put(static_cast<int>(x), y, live); }
        last = y;
        if (mode == TraceMode::MinHold) {
            st.minHold[x] = std::min(st.minHold[x], v);
            heldY[x] = yOf(st.minHold[x]);
        } else if (mode == TraceMode::PeakHold) {
            st.peak[x] = std::max(st.peak[x], v);
            heldY[x] = yOf(st.peak[x]);
        } else if (mode == TraceMode::Average) {
            const float a = std::clamp(st.averageAlpha, 0.001f, 1.0f);
            st.average[x] = st.average[x] <= -299.0f ? v : (a * v + (1.0f - a) * st.average[x]);
            heldY[x] = yOf(st.average[x]);
        }
    }
    // the bold held trace over it
    if (mode != TraceMode::Normal) {
        const Rgba c = mode == TraceMode::PeakHold ? held : average;   // (min hold takes the average's colour: the held lines are told apart by the label in the header)
        int prev = -1;
        for (std::uint32_t x = 0; x < w; ++x) {
            const int y = heldY[x];
            if (prev >= 0) { vline(static_cast<int>(x), prev, y, c); vline(static_cast<int>(x), prev - 1, y - 1, c); }
            else { put(static_cast<int>(x), y, c); put(static_cast<int>(x), y - 1, c); }
            prev = y;
        }
    }
}

void scrollWaterfallRow(Rgba* px, std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax, const Rgba* lut) {
    if (px == nullptr || w == 0 || h == 0) { return; }
    if (h > 1) { std::memmove(px + w, px, sizeof(Rgba) * static_cast<std::size_t>(w) * (h - 1)); }
    const double range = std::max(1.0, dbMax - dbMin);
    for (std::uint32_t x = 0; x < w; ++x) {
        const float v = (bins != nullptr && n > 0) ? columnMax(bins, n, x, w) : -300.0f;
        const float t = static_cast<float>((v - dbMin) / range);
        px[x] = lut != nullptr ? lut[t <= 0.0f ? 0 : t >= 1.0f ? 255 : static_cast<std::size_t>(t * 255.0f + 0.5f)] : heat(t);
    }
}

// ---- status cards ----
std::vector<StatusCardModel> statusCards(const FoxReceiverState& s, const HostExtras& x) {
    std::vector<StatusCardModel> cards;
    const bool running = (s.flags & FOXAPI_RX_RUNNING) != 0;
    const bool faulted = (s.flags & FOXAPI_RX_FAULTED) != 0;

    {   // RECEIVER
        StatusCardModel c;
        c.key = cascade::i18n::tr("RECEIVER");
        c.lampShown = true;
        if (faulted) { c.value = cascade::i18n::tr("FAULT"); c.tone = CardTone::Err; c.lampLit = true; c.lampRed = true; }
        else if (running) { c.value = cascade::i18n::tr("RUNNING"); c.tone = CardTone::Good; c.lampLit = true; }
        else { c.value = cascade::i18n::tr("STOPPED"); }
        const std::string ant = x.antenna.empty() ? std::string() : cascade::core::formatText(cascade::i18n::tr("ANT %s"), x.antenna.c_str());
        const std::string gain = x.gainText.empty() ? std::string() : cascade::core::formatText(cascade::i18n::tr("GAIN %s"), x.gainText.c_str());
        const std::string ppm = x.havePpm ? fmt("PPM %.1f", x.ppm) : std::string();
        c.detail = joinDot({s.deviceName, ant, gain, ppm, fmtRate(s.sampleRateHz)});
        c.lines = {s.deviceName};
        if (!x.antenna.empty()) { c.lines.push_back(cascade::core::formatText(cascade::i18n::tr("ANTENNA %s"), x.antenna.c_str())); }
        // x.gainText is compared with the English "AUTO" (and main.cpp compares gainText() with "N/A"): those two words stay English upstream
        if (!x.gainText.empty()) { c.lines.push_back(x.gainText == "AUTO" ? std::string(cascade::i18n::tr("GAIN AUTO")) : cascade::core::formatText(cascade::i18n::tr("GAIN %s"), x.gainText.c_str())); }
        if (x.havePpm) { c.lines.push_back(ppm); }
        c.lines.push_back(fmtRate(s.sampleRateHz));
        if (faulted && s.faultMessage[0] != '\0') { c.lines.push_back(s.faultMessage); }
        cards.push_back(c);
    }
    {   // DECODERS
        StatusCardModel c;
        c.key = cascade::i18n::tr("DECODERS");
        const unsigned fitted = s.decodersFitted, runningN = s.decodersRunning, matched = s.decodersMatched;
        const auto z = [](unsigned v) { return static_cast<std::size_t>(v); };
        if (runningN > 0) {
            c.value = cascade::core::formatText(cascade::i18n::tr("%zu running"), z(runningN));
            c.tone = CardTone::Good;
            const unsigned idle = matched > runningN ? matched - runningN : 0;
            c.detail = idle > 0 ? cascade::core::formatText(cascade::i18n::tr("of %zu fitted, %zu idle"), z(fitted), z(idle)) : cascade::core::formatText(cascade::i18n::tr("of %zu fitted"), z(fitted));
        } else {
            c.value = cascade::core::formatText(cascade::i18n::tr("%zu ready"), z(matched));
            if (!running) { c.detail = cascade::core::formatText(cascade::i18n::tr("of %zu fitted - receiver stopped"), z(fitted)); }
            else { c.detail = matched > 0 ? cascade::core::formatText(cascade::i18n::tr("of %zu fitted, %zu not fed"), z(fitted), z(matched)) : cascade::core::formatText(cascade::i18n::tr("of %zu fitted"), z(fitted)); }
        }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // OUTPUT
        StatusCardModel c;
        c.key = cascade::i18n::tr("OUTPUT");
        if (!running) { c.value = "--"; c.detail = cascade::i18n::tr("the receiver is stopped"); }
        else if (s.decodersRunning == 0) { c.value = "--"; c.detail = cascade::i18n::tr("no decoder is running"); }
        else if (!x.outputMeasured) { c.value = "--"; c.detail = cascade::i18n::tr("measuring"); }
        else { c.value = fmt("%.1f /s", x.outputRate); c.detail = cascade::core::formatText(cascade::i18n::tr("output lines, %.1f s mean"), x.outputMeanS); c.tone = CardTone::Warn; }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // SINK
        StatusCardModel c;
        c.key = cascade::i18n::tr("SINK");
        const bool open = (s.flags & FOXAPI_RX_SINK_OPEN) != 0;
        const bool muted = (s.flags & FOXAPI_RX_MUTED) != 0 || s.audioMutedBy[0] != '\0';
        const bool opening = (s.audioFlags & FOXAPI_AUDIO_OPENING) != 0;
        const bool everOpened = (s.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0;
        if (opening) { c.value = cascade::i18n::tr("OPENING"); c.detail = cascade::i18n::tr("waiting for the output device"); c.lines = {c.detail}; }
        else if (open) {
            c.value = muted ? cascade::i18n::tr("MUTED") : cascade::i18n::tr("OPEN");
            c.tone = muted ? CardTone::Warn : CardTone::Good;
            const std::string playing = s.audioPlayingPlugin[0] != '\0' ? cascade::core::formatText(cascade::i18n::tr("playing: %s"), s.audioPlayingPlugin) : cascade::core::formatText(cascade::i18n::tr("playing %s"), modeName(s.demodMode));
            c.detail = joinDot({s.sinkName, playing});
            c.lines = {s.sinkName};
            if (s.audioMutedBy[0] != '\0') { c.lines.push_back(cascade::core::formatText(cascade::i18n::tr("by %s"), s.audioMutedBy)); }
            if (s.audioPlayingPlugin[0] != '\0') { c.lines.push_back(cascade::core::formatText(cascade::i18n::tr("for %s"), s.audioPlayingPlugin)); }
            c.lines.push_back(playing);
        } else if (!running) { c.value = cascade::i18n::tr("STOPPED"); c.detail = s.sinkName; c.lines = {s.sinkName}; }
        else if (everOpened) { c.value = cascade::i18n::tr("STOPPED"); c.tone = CardTone::Err; c.detail = cascade::i18n::tr("the output stream stopped"); c.lines = {c.detail}; }
        else { c.value = cascade::i18n::tr("NO DEVICE"); c.tone = CardTone::Err; c.detail = cascade::i18n::tr("no output device was opened"); c.lines = {c.detail}; }
        cards.push_back(c);
    }
    {   // RECORDER
        StatusCardModel c;
        c.key = cascade::i18n::tr("RECORDER");
        const bool iq = (s.flags & FOXAPI_RX_RECORDING_IQ) != 0, audio = (s.flags & FOXAPI_RX_RECORDING_AUDIO) != 0;
        if (iq || audio) {
            c.value = !x.recorderValue.empty() ? x.recorderValue : (iq && audio ? cascade::i18n::tr("IQ + AUDIO") : iq ? "IQ" : cascade::i18n::tr("AUDIO"));   // "IQ" is radio vocabulary, not translated
            c.detail = x.recorderDetail;
            c.tone = CardTone::Err;
        } else { c.value = cascade::i18n::tr("off"); c.detail = cascade::i18n::tr("no file open"); }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // WEB
        StatusCardModel c;
        c.key = cascade::i18n::tr("WEB");
        if (!x.webRefused.empty()) { c.value = cascade::i18n::tr("REFUSED"); c.detail = x.webRefused; c.tone = CardTone::Err; }
        else if (s.flags & FOXAPI_RX_WEB_LISTENING) { c.value = cascade::core::formatText(cascade::i18n::tr("port %d"), x.webPort); c.detail = x.webBind; }
        else { c.value = cascade::i18n::tr("off"); c.detail = cascade::i18n::tr("not listening"); }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // UNDERRUNS
        StatusCardModel c;
        c.key = cascade::i18n::tr("UNDERRUNS");
        c.value = std::to_string(s.audioUnderruns);
        if (s.audioUnderruns == 0) { c.detail = cascade::i18n::tr("no callback has starved yet"); c.lines = {c.detail}; }
        else {
            c.tone = CardTone::Err;
            c.detail = cascade::i18n::tr("starved callbacks, since start");
            c.lines = {c.detail};
            if (s.audioRateHz > 0.0 && s.audioRingCapacityFrames > 0) {
                c.lines.push_back(cascade::core::formatText(cascade::i18n::tr("ring %.0f of %.0f ms"), 1000.0 * static_cast<double>(s.audioRingFrames) / s.audioRateHz,
                                      1000.0 * static_cast<double>(s.audioRingCapacityFrames) / s.audioRateHz));
            }
            if (s.audioPluginGaps > 0 || s.audioPlayingPlugin[0] != '\0') {
                c.lines.push_back(cascade::core::formatText(cascade::i18n::tr("plugin gaps %llu, %llu frames"), static_cast<unsigned long long>(s.audioPluginGaps), static_cast<unsigned long long>(s.audioPluginGapFrames)));
            }
        }
        cards.push_back(c);
    }
    return cards;
}

// ---- the output rate ----
void OutputRate::add(std::uint64_t total, std::int64_t nowMs) {
    samples_.push_back({total, nowMs});
    // drop the oldest while the window without it still spans at least two seconds (never below two samples)
    while (samples_.size() > 2 && (samples_.back().ms - samples_[1].ms) >= kWindowMs) { samples_.pop_front(); }
}
bool OutputRate::measured() const { return samples_.size() >= 2 && (samples_.back().ms - samples_.front().ms) >= kWindowMs; }
double OutputRate::perSecond() const {
    if (samples_.size() < 2) { return 0.0; }
    const double span = static_cast<double>(samples_.back().ms - samples_.front().ms) / 1000.0;
    if (!(span > 0.0)) { return 0.0; }
    const double lines = static_cast<double>(samples_.back().total) - static_cast<double>(samples_.front().total);
    return std::max(0.0, lines / span);
}
double OutputRate::meanSeconds() const {
    if (samples_.size() < 2) { return 0.0; }
    return static_cast<double>(samples_.back().ms - samples_.front().ms) / 1000.0;
}
void OutputRate::reset() { samples_.clear(); }

// ---- lines and hints ----
std::string rateLine(const FoxReceiverState& s) {
    return joinDot({fmtRate(s.sampleRateHz), cascade::core::formatText(cascade::i18n::tr("ch %s"), fmtKHz(s.channelRateHz).c_str()), cascade::core::formatText(cascade::i18n::tr("underruns %llu"), static_cast<unsigned long long>(s.audioUnderruns))});
}
std::string sourceLine(const FoxReceiverState& s, const std::string& band) { return joinDot({s.deviceName, band}); }
std::string modeHint(const FoxReceiverState& s) {
    return joinDot({cascade::core::formatText(cascade::i18n::tr("Passband %s"), fmtKHz(s.bandwidthHz).c_str()), cascade::core::formatText(cascade::i18n::tr("step %s"), fmtKHz(modeStepHz(s.demodMode)).c_str()), cascade::i18n::tr("more in Signal \xE2\x80\xBA Radio")});
}
std::string tuneHint(bool converterActive) {
    return converterActive ? std::string(cascade::i18n::tr("Scroll a digit to tune \xC2\xB7 click to type (T)"))
                           : std::string(cascade::i18n::tr("Scroll a digit to tune \xC2\xB7 click to type (T) \xC2\xB7 no converter"));
}
std::string biasHint(bool present, bool on) {
    if (!present) { return cascade::i18n::tr("No bias tee on this radio"); }
    return on ? std::string(cascade::i18n::tr("On \xC2\xB7 4.5 V on the cable")) : std::string(cascade::i18n::tr("Off \xC2\xB7 4.5 V on the cable \xC2\xB7 asks first"));
}
std::vector<int> bookmarkNameRows(const std::vector<BookmarkName>& marks, double widthPx, double charPx, int rows) {
    std::vector<int> out(marks.size(), -1);
    if (rows < 1) { rows = 1; }
    if (rows > kBookmarkNameMaxRows) { rows = kBookmarkNameMaxRows; }
    std::vector<std::vector<std::pair<double, double>>> placedByRow(static_cast<std::size_t>(rows));
    // favourites first (pass 0), then the rest, as AppWindow::drawBookmarkMarkers walks them
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < marks.size(); ++i) {
            const BookmarkName& b = marks[i];
            if ((pass == 0) != b.favourite || b.label.empty()) { continue; }
            const double x = b.pos * widthPx;
            const double a = x + 3.0, e = a + charPx * static_cast<double>(b.label.size());
            if (e > widthPx) { continue; }   // it would run off the right edge
            for (std::size_t r = 0; r < placedByRow.size(); ++r) {
                bool clash = false;
                for (const auto& p : placedByRow[r]) {
                    if (a < p.second + kBookmarkNameGapPx && e + kBookmarkNameGapPx > p.first) { clash = true; break; }
                }
                if (!clash) {
                    out[i] = static_cast<int>(r);
                    placedByRow[r].emplace_back(a, e);
                    break;
                }
            }
        }
    }
    return out;
}

std::string waterfallFootRight(const FoxReceiverState& s, const std::string& decoder) {
    if (!(s.flags & FOXAPI_RX_RUNNING)) { return cascade::i18n::tr("STOPPED"); }
    return joinDot({modeName(s.demodMode), decoder.empty() ? std::string(cascade::i18n::tr("receiving")) : cascade::core::formatText(cascade::i18n::tr("decoding: %s"), decoder.c_str())});
}

}  // namespace foxsdr::slinthost
