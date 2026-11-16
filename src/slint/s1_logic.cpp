// s1_logic.cpp - see s1_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s1_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/i18n.hpp"
#include "dsp/tone_squelch.hpp"
#include "lanel_logic.hpp"

namespace foxsdr::slints1 {

namespace {
namespace tone = cascade::dsp::tone;
using foxsdr::slintlanel::numberText;
using foxsdr::slintlanel::parseNumber;
using foxsdr::slintlanel::trim;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }

std::string fmt1(const char* f, double v) { char b[64]; std::snprintf(b, sizeof(b), f, v); return b; }
}  // namespace

// ============================================================ ranges ============================================================
double fromLevel(const Range& r, double level) {
    if (!std::isfinite(level)) { return r.lo; }
    return r.lo + std::clamp(level, 0.0, 1.0) * (r.hi - r.lo);
}
float toLevel(const Range& r, double value) {
    if (!std::isfinite(value) || r.hi <= r.lo) { return 0.0f; }
    return static_cast<float>(std::clamp((value - r.lo) / (r.hi - r.lo), 0.0, 1.0));
}
double fromLevelStep(const Range& r, double level, double step) {
    const double v = fromLevel(r, level);
    if (!(step > 0.0)) { return v; }
    return std::clamp(std::round(v / step) * step, r.lo, r.hi);
}

// ============================================================ the meter ============================================================
std::vector<std::string> meterUnitNames() { return {tr("Level in dB"), tr("S units"), tr("Level in dBm")}; }

// ============================================================ passband ============================================================
bool isFmMode(unsigned m) { return m == FOXAPI_DEMOD_NFM || m == FOXAPI_DEMOD_WFM; }

std::vector<double> widthPresets(unsigned m) {
    switch (m) {
        case FOXAPI_DEMOD_NFM:
        case FOXAPI_DEMOD_WFM: return {12500.0, 30000.0, 50000.0, 150000.0, 225000.0};   // core/receiver_tables.hpp kFmWidthHz
        case FOXAPI_DEMOD_AM: return {3000.0, 4000.0, 6000.0, 9000.0, 10000.0, 15000.0};
        case FOXAPI_DEMOD_DSB: return {3000.0, 6000.0, 10000.0, 15000.0};
        case FOXAPI_DEMOD_USB:
        case FOXAPI_DEMOD_LSB: return {3000.0, 4000.0, 6000.0};
        case FOXAPI_DEMOD_CW: return {3000.0, 4000.0, 6000.0};
        case FOXAPI_DEMOD_RAW:
        default: return {12500.0, 50000.0, 150000.0, 200000.0};
    }
}

std::string widthText(double hz) {
    if (hz >= 1000.0) {
        std::string s = fmt1("%.3f", hz / 1000.0);
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
        return s + " kHz";
    }
    return fmt1("%.0f", hz) + " Hz";
}

int presetIndexOf(const std::vector<double>& p, double bw) {
    for (std::size_t i = 0; i < p.size(); ++i) { if (std::fabs(p[i] - bw) < 1.0) { return static_cast<int>(i); } }
    return -1;
}

std::vector<std::string> fmWidthNames() {
    return {tr("Narrow") + " 12.5 kHz", tr("Medium") + " 30 kHz", tr("Medium-wide") + " 50 kHz", tr("Broadcast") + " 150 kHz", tr("Super-wide") + " 225 kHz"};
}

std::vector<std::string> shapeNames() { return {tr("Default filter"), tr("Sharp filter"), tr("Medium filter"), tr("Soft filter")}; }

// ============================================================ squelch and tone ============================================================
bool parseCtcss(const std::string& text, double& hz, std::string& err) {
    const std::string t = trim(text);
    if (t.empty() || t == "0") { hz = 0.0; return true; }
    double v = 0.0;
    std::string e;
    if (!parseNumber(t, 0.0, 300.0, v, e)) { err = e; return false; }
    if (!tone::validCtcssHz(v)) { err = tr("that is not one of the 50 CTCSS tones"); return false; }
    // the engine's own list spells the tone; a tone typed to a tenth is held to it
    hz = v == 0.0 ? 0.0 : cascade::dsp::ctcssToneHz(cascade::dsp::ctcssIndexOfHz(v));
    return true;
}

bool parseDcs(const std::string& text, int& digits, std::string& err) {
    const std::string t = trim(text);
    if (t.empty() || t == "0") { digits = 0; return true; }
    if (t.size() > 3) { err = tr("that is not one of the 83 standard DCS codes"); return false; }
    for (const char c : t) { if (c < '0' || c > '7') { err = tr("a DCS code is written in the digits 0 to 7, like 023"); return false; } }
    const int v = std::atoi(t.c_str());
    if (!tone::validDcsDigits(v)) { err = tr("that is not one of the 83 standard DCS codes"); return false; }
    digits = v;
    return true;
}

double nextCtcss(double hz, int direction) {
    const int n = cascade::dsp::kCtcssAllCount;
    const int at = hz <= 0.0 ? -1 : cascade::dsp::ctcssIndexOfHz(hz);
    int to = direction >= 0 ? at + 1 : (at < 0 ? n - 1 : at - 1);   // from "any" (none chosen) forwards starts at the first tone, backwards at the last
    if (to < 0) { to = n - 1; }
    if (to >= n) { to = 0; }
    return cascade::dsp::ctcssToneHz(to);
}

int nextDcs(int digits, int direction) {
    const auto& codes = cascade::dsp::kDcsStandardCodes;
    const int n = static_cast<int>(codes.size());
    int at = -1;
    for (int i = 0; i < n; ++i) { if (tone::octalToDigits(codes[static_cast<std::size_t>(i)]) == digits) { at = i; break; } }
    int to = direction >= 0 ? (at < 0 ? 0 : at + 1) : (at < 0 ? n - 1 : at - 1);
    if (to < 0) { to = n - 1; }
    if (to >= n) { to = 0; }
    return tone::octalToDigits(codes[static_cast<std::size_t>(to)]);
}

std::string dcsText(int digits) { char b[16]; std::snprintf(b, sizeof(b), "%03d", digits); return b; }
std::string ctcssText(double hz) { return fmt1("%.1f", hz); }

std::vector<std::string> toneKindNames() { return {tr("Tone squelch off"), tr("CTCSS tone squelch"), tr("DCS code squelch")}; }
std::vector<std::string> polarityNames() { return {tr("Any polarity"), tr("Normal polarity"), tr("Inverted polarity")}; }

// ============================================================ audio ============================================================
std::vector<std::string> eqBandNames() {
    return {tr("Bass"), tr("Middle"), tr("Treble"), "100 Hz", "300 Hz", "1 kHz", "2.5 kHz", "5 kHz"};
}

const int kHighPassHz[3] = {0, 100, 300};
const int kLowPassHz[4] = {0, 2400, 3000, 6000};

int indexOfHighPass(unsigned hz) { for (int i = 0; i < 3; ++i) { if (static_cast<unsigned>(kHighPassHz[i]) == hz) { return i; } } return -1; }
int indexOfLowPass(unsigned hz) { for (int i = 0; i < 4; ++i) { if (static_cast<unsigned>(kLowPassHz[i]) == hz) { return i; } } return -1; }

std::vector<std::string> highPassNames() { return {tr("High-pass off"), tr("High-pass") + " 100 Hz", tr("High-pass") + " 300 Hz"}; }
std::vector<std::string> lowPassNames() { return {tr("Low-pass off"), tr("Low-pass") + " 2.4 kHz", tr("Low-pass") + " 3 kHz", tr("Low-pass") + " 6 kHz"}; }
std::vector<std::string> nrMethodNames() { return {tr("Standard noise reduction"), tr("Spectral noise reduction"), tr("Line enhancer")}; }
std::vector<std::string> placementNames() { return {tr("Placement off"), tr("Pan"), tr("Pseudo-stereo"), tr("Binaural")}; }

Range placementRange(int mode) {
    switch (mode) {
        case 1: return {-1.0, 1.0};
        case 2: return {0.0, 1.0};
        case 3: return {-90.0, 90.0};
        default: return {0.0, 1.0};
    }
}
double placementDefault(int mode) { return mode == 2 ? 0.5 : 0.0; }
std::string placementText(int mode, double v) {
    switch (mode) {
        case 1: return v < -0.005 ? fmt1("%.0f", -v * 100.0) + "% " + tr("left") : v > 0.005 ? fmt1("%.0f", v * 100.0) + "% " + tr("right") : tr("centre");
        case 2: return fmt1("%.0f", v * 100.0) + "%";
        case 3: return fmt1("%+.0f", v) + " " + tr("degrees");
        default: return std::string();
    }
}

bool parseNotchSlot(const std::string& text, int& slot0, std::string& err) {
    double v = 0.0;
    std::string e;
    if (!parseNumber(text, 1.0, static_cast<double>(kNotchSlots), v, e) || std::fabs(v - std::round(v)) > 1e-9) {
        err = e.empty() ? tr("a notch slot is a whole number from 1 to 10") : e;
        return false;
    }
    slot0 = static_cast<int>(std::lround(v)) - 1;
    return true;
}
bool parseNotchHz(const std::string& text, double& hz, std::string& err) { return parseNumber(text, kNotchHz.lo, kNotchHz.hi, hz, err); }
bool parseNotchWidth(const std::string& text, double& hz, std::string& err) { return parseNumber(text, kNotchWidth.lo, kNotchWidth.hi, hz, err); }

std::string notchSummary(unsigned mask, const double* hz, const double* width) {
    std::string out;
    for (int i = 0; i < kNotchSlots; ++i) {
        if ((mask & (1u << i)) == 0u) { continue; }
        if (!out.empty()) { out += "; "; }
        out += std::to_string(i + 1) + ": " + numberText(std::round(hz[i] * 10.0) / 10.0) + " Hz, " + tr("wide") + " " + numberText(std::round(width[i] * 10.0) / 10.0) + " Hz";
    }
    return out;
}

// ============================================================ I/Q correction ============================================================
std::string imageRejectionText(double db) {
    if (!std::isfinite(db) || db >= 199.0) { return std::string(); }
    return fmt1("%.1f", db) + " dB";
}

// ============================================================ the passband's edges and the AM detector ============================================================
namespace {
std::vector<std::string> splitSemi(const std::string& t) {
    std::vector<std::string> v;
    std::size_t at = 0;
    for (;;) {
        const std::size_t e = t.find(';', at);
        v.push_back(t.substr(at, e == std::string::npos ? std::string::npos : e - at));
        if (e == std::string::npos) { break; }
        at = e + 1;
    }
    return v;
}
}  // namespace

PassbandFact parsePassbandFact(const std::string& text) {
    PassbandFact f;
    const auto v = splitSemi(text);
    if (v.size() != 5) { return f; }
    f.on = v[0] == "1";
    f.low = std::atof(v[1].c_str());
    f.high = std::atof(v[2].c_str());
    f.shift = std::atof(v[3].c_str());
    f.active = v[4] == "1";
    f.ok = true;
    return f;
}

bool hasPassband(unsigned m) { return m == FOXAPI_DEMOD_USB || m == FOXAPI_DEMOD_LSB || m == FOXAPI_DEMOD_CW || m == FOXAPI_DEMOD_AM; }

void defaultEdges(unsigned m, double pitch, double& low, double& high) {
    if (m == FOXAPI_DEMOD_CW) {
        const double p = std::isfinite(pitch) ? pitch : 700.0;
        low = std::max(0.0, p - 250.0);
        high = p + 250.0;
    } else if (m == FOXAPI_DEMOD_AM) {
        low = 4500.0;
        high = 4500.0;
    } else {
        low = 300.0;
        high = 2700.0;
    }
}

bool checkCuts(unsigned m, double low, double high, std::string& err) {
    if (m != FOXAPI_DEMOD_AM && !(high > low)) { err = tr("the high cut has to be above the low cut"); return false; }
    return true;
}

AmDetFact parseAmDetFact(const std::string& text) {
    AmDetFact f;
    const auto v = splitSemi(text);
    if (v.size() != 4) { return f; }
    f.kind = std::atoi(v[0].c_str());
    f.locked = v[1] == "1";
    f.offsetHz = std::atof(v[2].c_str());
    f.presence = std::atof(v[3].c_str());
    f.ok = f.kind >= 0 && f.kind <= 3;
    return f;
}

std::vector<std::string> amDetectorNames() {
    return {tr("Envelope detector"), tr("Synchronous detector, both sidebands"), tr("Synchronous detector, upper sideband"), tr("Synchronous detector, lower sideband")};
}

// ============================================================ the engine's state by name ============================================================
bool stateValue(const FoxReceiverState& s, const std::string& n, double& out) {
    const auto indexed = [&](const char* prefix, int count, const double* arr) {
        const std::size_t len = std::char_traits<char>::length(prefix);
        if (n.compare(0, len, prefix) != 0) { return false; }
        const int i = std::atoi(n.c_str() + len);
        if (i < 0 || i >= count) { return false; }
        out = arr[i];
        return true;
    };
    if (indexed("eqDb.", kEqBands, s.eqDb)) { return true; }
    if (indexed("notchBankHz.", kNotchSlots, s.notchBankHz)) { return true; }
    if (indexed("notchBankWidthHz.", kNotchSlots, s.notchBankWidthHz)) { return true; }
    struct F { const char* name; double v; };
    const F fields[] = {
        {"cwPitchHz", s.cwPitchHz}, {"meterUnits", s.meterUnits}, {"toneKind", s.toneKind}, {"toneHz", s.toneHz}, {"toneCode", s.toneCode}, {"tonePolarity", s.tonePolarity}, {"toneRemove", s.toneRemove},
        {"noiseSquelchOn", s.noiseSquelchOn}, {"noiseSquelchThresholdDb", s.noiseSquelchThresholdDb}, {"noiseSquelchTailMs", s.noiseSquelchTailMs},
        {"nrMethod", s.nrMethod}, {"audioHighPassHz", s.audioHighPassHz}, {"audioLowPassHz", s.audioLowPassHz}, {"notchBankMask", s.notchBankMask},
        {"stereoPlaceMode", s.stereoPlaceMode}, {"stereoPlaceValue", s.stereoPlaceValue}, {"cwPeakOn", s.cwPeakOn}, {"cwPeakWidthHz", s.cwPeakWidthHz}, {"cwPeakBoostDb", s.cwPeakBoostDb},
        {"afcOn", s.afcOn}, {"afcPullInHz", s.afcPullInHz}, {"iqCorrectMask", s.iqCorrectMask}, {"filterShape", s.filterShape}, {"bandwidthHz", s.bandwidthHz}, {"demodMode", s.demodMode},
        {"signalDbm", s.signalDbm}, {"sUnits", s.sUnits}, {"calibrated", s.calibrated},
    };
    for (const F& f : fields) { if (n == f.name) { out = f.v; return true; } }
    return false;
}

}  // namespace foxsdr::slints1
