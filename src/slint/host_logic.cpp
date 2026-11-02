// The Slint host's pure logic - see host_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace foxsdr::slinthost {
namespace {

const char* const kDot = " \xC2\xB7 ";   // the IA's middle dot between clauses

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return b;
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

// ---- readouts ----
std::string fmtMHz(double hz) { return fmt("%.6f MHz", hz / 1e6); }
std::string fmtRate(double hz) { return hz > 0.0 ? fmt("%.3f MS/s", hz / 1e6) : std::string("--"); }
std::string fmtKHz(double hz) { return fmt("%.1f kHz", hz / 1e3); }
std::string spanText(double hz) {
    if (hz >= 1e9) { return "SPAN " + fmt("%.3f GHz", hz / 1e9); }
    if (hz >= 1e6) { return "SPAN " + fmt("%.3f MHz", hz / 1e6); }
    if (hz >= 1e3) { return "SPAN " + fmt("%.1f kHz", hz / 1e3); }
    return "SPAN " + fmt("%.0f Hz", hz);
}
std::string cursorText(double hz, double db) { return fmt("%.3f MHz", hz / 1e6) + kDot + fmt("%.1f dB", db); }
std::string axisUnit(double) { return "MHz"; }

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
    const int labelEvery = pitch < 40.0 ? 20 : 10;
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
    out.push_back({0.0, "now"});
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
}

void paintTrace(Rgba* px, std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax,
                TraceState& st, TraceMode mode, Rgba live, Rgba held, Rgba average) {
    if (px == nullptr || w == 0 || h == 0) { return; }
    std::fill(px, px + static_cast<std::size_t>(w) * h, Rgba{0, 0, 0, 0});
    if (bins == nullptr || n == 0) { return; }
    const double range = std::max(1.0, dbMax - dbMin);
    const auto yOf = [&](float db) { return static_cast<int>((1.0 - std::clamp((db - dbMin) / range, 0.0, 1.0)) * (h - 1)); };
    const auto put = [&](int x, int y, Rgba c) {
        if (x >= 0 && y >= 0 && x < static_cast<int>(w) && y < static_cast<int>(h)) { px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = c; }
    };
    const auto vline = [&](int x, int y0, int y1, Rgba c) { for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) { put(x, y, c); } };

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
        if (last >= 0) { vline(static_cast<int>(x), last, y, live); } else { put(static_cast<int>(x), y, live); }
        last = y;
        if (mode == TraceMode::PeakHold) {
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
        const Rgba c = mode == TraceMode::PeakHold ? held : average;
        int prev = -1;
        for (std::uint32_t x = 0; x < w; ++x) {
            const int y = heldY[x];
            if (prev >= 0) { vline(static_cast<int>(x), prev, y, c); vline(static_cast<int>(x), prev - 1, y - 1, c); }
            else { put(static_cast<int>(x), y, c); put(static_cast<int>(x), y - 1, c); }
            prev = y;
        }
    }
}

void scrollWaterfallRow(Rgba* px, std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax) {
    if (px == nullptr || w == 0 || h == 0) { return; }
    if (h > 1) { std::memmove(px + w, px, sizeof(Rgba) * static_cast<std::size_t>(w) * (h - 1)); }
    const double range = std::max(1.0, dbMax - dbMin);
    for (std::uint32_t x = 0; x < w; ++x) {
        const float v = (bins != nullptr && n > 0) ? columnMax(bins, n, x, w) : -300.0f;
        px[x] = heat(static_cast<float>((v - dbMin) / range));
    }
}

// ---- status cards ----
std::vector<StatusCardModel> statusCards(const FoxReceiverState& s, const HostExtras& x) {
    std::vector<StatusCardModel> cards;
    const bool running = (s.flags & FOXAPI_RX_RUNNING) != 0;
    const bool faulted = (s.flags & FOXAPI_RX_FAULTED) != 0;

    {   // RECEIVER
        StatusCardModel c;
        c.key = "RECEIVER";
        c.lampShown = true;
        if (faulted) { c.value = "FAULT"; c.tone = CardTone::Err; c.lampLit = true; c.lampRed = true; }
        else if (running) { c.value = "RUNNING"; c.tone = CardTone::Good; c.lampLit = true; }
        else { c.value = "STOPPED"; }
        const std::string ant = x.antenna.empty() ? std::string() : ("ANT " + x.antenna);
        const std::string gain = x.gainText.empty() ? std::string() : ("GAIN " + x.gainText);
        const std::string ppm = fmt("PPM %.1f", x.ppm);
        c.detail = joinDot({s.deviceName, ant, gain, ppm, fmtRate(s.sampleRateHz)});
        c.lines = {s.deviceName};
        if (!x.antenna.empty()) { c.lines.push_back("ANTENNA " + x.antenna); }
        if (!x.gainText.empty()) { c.lines.push_back(x.gainText == "AUTO" ? std::string("GAIN AUTO - AGC ON") : "GAIN " + x.gainText); }
        c.lines.push_back(ppm);
        c.lines.push_back(fmtRate(s.sampleRateHz));
        if (faulted && s.faultMessage[0] != '\0') { c.lines.push_back(s.faultMessage); }
        cards.push_back(c);
    }
    {   // DECODERS
        StatusCardModel c;
        c.key = "DECODERS";
        const unsigned fitted = s.decodersFitted, runningN = s.decodersRunning, matched = s.decodersMatched;
        if (runningN > 0) {
            c.value = std::to_string(runningN) + " running";
            c.tone = CardTone::Good;
            const unsigned idle = matched > runningN ? matched - runningN : 0;
            c.detail = "of " + std::to_string(fitted) + " fitted" + (idle > 0 ? ", " + std::to_string(idle) + " idle" : std::string());
        } else {
            c.value = std::to_string(matched) + " ready";
            if (!running) { c.detail = "of " + std::to_string(fitted) + " fitted - receiver stopped"; }
            else { c.detail = "of " + std::to_string(fitted) + " fitted" + (matched > 0 ? ", " + std::to_string(matched) + " not fed" : std::string()); }
        }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // OUTPUT
        StatusCardModel c;
        c.key = "OUTPUT";
        if (!running) { c.value = "--"; c.detail = "the receiver is stopped"; }
        else if (s.decodersRunning == 0) { c.value = "--"; c.detail = "no decoder is running"; }
        else if (!x.outputMeasured) { c.value = "--"; c.detail = "measuring"; }
        else { c.value = fmt("%.1f /s", x.outputRate); c.detail = fmt("output lines, %.1f s mean", x.outputMeanS); c.tone = CardTone::Warn; }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // SINK
        StatusCardModel c;
        c.key = "SINK";
        const bool open = (s.flags & FOXAPI_RX_SINK_OPEN) != 0;
        const bool muted = (s.flags & FOXAPI_RX_MUTED) != 0 || s.audioMutedBy[0] != '\0';
        const bool opening = (s.audioFlags & FOXAPI_AUDIO_OPENING) != 0;
        const bool everOpened = (s.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0;
        if (opening) { c.value = "OPENING"; c.detail = "waiting for the output device"; c.lines = {c.detail}; }
        else if (open) {
            c.value = muted ? "MUTED" : "OPEN";
            c.tone = muted ? CardTone::Warn : CardTone::Good;
            const std::string playing = s.audioPlayingPlugin[0] != '\0' ? std::string("playing: ") + s.audioPlayingPlugin : std::string("playing ") + modeName(s.demodMode);
            c.detail = joinDot({s.sinkName, playing});
            c.lines = {s.sinkName};
            if (s.audioMutedBy[0] != '\0') { c.lines.push_back(std::string("by ") + s.audioMutedBy); }
            if (s.audioPlayingPlugin[0] != '\0') { c.lines.push_back(std::string("for ") + s.audioPlayingPlugin); }
            c.lines.push_back(playing);
        } else if (!running) { c.value = "STOPPED"; c.detail = s.sinkName; c.lines = {s.sinkName}; }
        else if (everOpened) { c.value = "STOPPED"; c.tone = CardTone::Err; c.detail = "the output stream stopped"; c.lines = {c.detail}; }
        else { c.value = "NO DEVICE"; c.tone = CardTone::Err; c.detail = "no output device was opened"; c.lines = {c.detail}; }
        cards.push_back(c);
    }
    {   // RECORDER
        StatusCardModel c;
        c.key = "RECORDER";
        const bool iq = (s.flags & FOXAPI_RX_RECORDING_IQ) != 0, audio = (s.flags & FOXAPI_RX_RECORDING_AUDIO) != 0;
        if (iq || audio) {
            c.value = !x.recorderValue.empty() ? x.recorderValue : (iq && audio ? "IQ + AUDIO" : iq ? "IQ" : "AUDIO");
            c.detail = x.recorderDetail;
            c.tone = CardTone::Err;
        } else { c.value = "off"; c.detail = "no file open"; }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // WEB
        StatusCardModel c;
        c.key = "WEB";
        if (!x.webRefused.empty()) { c.value = "REFUSED"; c.detail = x.webRefused; c.tone = CardTone::Err; }
        else if (s.flags & FOXAPI_RX_WEB_LISTENING) { c.value = "port " + std::to_string(x.webPort); c.detail = x.webBind; }
        else { c.value = "off"; c.detail = "not listening"; }
        c.lines = {c.detail};
        cards.push_back(c);
    }
    {   // UNDERRUNS
        StatusCardModel c;
        c.key = "UNDERRUNS";
        c.value = std::to_string(s.audioUnderruns);
        if (s.audioUnderruns == 0) { c.detail = "no callback has starved yet"; c.lines = {c.detail}; }
        else {
            c.tone = CardTone::Err;
            c.detail = "starved callbacks, since start";
            c.lines = {c.detail};
            if (s.audioRateHz > 0.0 && s.audioRingCapacityFrames > 0) {
                c.lines.push_back("ring " + fmt("%.0f", 1000.0 * static_cast<double>(s.audioRingFrames) / s.audioRateHz) + " of " +
                                  fmt("%.0f", 1000.0 * static_cast<double>(s.audioRingCapacityFrames) / s.audioRateHz) + " ms");
            }
            if (s.audioPluginGaps > 0 || s.audioPlayingPlugin[0] != '\0') {
                c.lines.push_back("plugin gaps " + std::to_string(s.audioPluginGaps) + ", " + std::to_string(s.audioPluginGapFrames) + " frames");
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
    return joinDot({fmtRate(s.sampleRateHz), "ch " + fmtKHz(s.channelRateHz), "underruns " + std::to_string(s.audioUnderruns)});
}
std::string sourceLine(const FoxReceiverState& s, const std::string& band) { return joinDot({s.deviceName, band}); }
std::string modeHint(const FoxReceiverState& s) {
    return joinDot({"Passband " + fmtKHz(s.bandwidthHz), "step " + fmtKHz(modeStepHz(s.demodMode)), "more in Signal \xE2\x80\xBA Radio"});
}
std::string tuneHint(bool converterActive) {
    return converterActive ? std::string("Scroll a digit to tune") + kDot + "click to type (T)"
                           : std::string("Scroll a digit to tune") + kDot + "click to type (T)" + kDot + "no converter";
}
std::string biasHint(bool present, bool on) {
    if (!present) { return "No bias tee on this radio"; }
    return on ? std::string("On") + kDot + "4.5 V on the cable" : std::string("Off") + kDot + "4.5 V on the cable" + kDot + "asks first";
}
std::string waterfallFootRight(const FoxReceiverState& s, const std::string& decoder) {
    if (!(s.flags & FOXAPI_RX_RUNNING)) { return "STOPPED"; }
    return joinDot({modeName(s.demodMode), decoder.empty() ? std::string("receiving") : "decoding: " + decoder});
}

}  // namespace foxsdr::slinthost
