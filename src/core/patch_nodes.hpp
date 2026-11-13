// patch_nodes.hpp - THE SETTINGS TEXT OF THE NEW PATCH PARTS (wire-a second pass). A node's `settings` (core/patch_graph.hpp) is one text; this file is where each kind's format is stated and read.
//
// IF FILTER (NodeKind::IfFilter): "low=<RF Hz>;high=<RF Hz>;notch=<RF Hz>/<width Hz>,<RF Hz>/<width Hz>,..." in any order, every part optional, numbers plain decimals. low and high together (both or neither) switch the pass-band on;
// up to dsp::IfFilter::kMaxNotches notches. An empty text is a filter with everything off: the I/Q passes untouched. The edges and the notch centres are absolute RF frequencies (the filter keeps them there when the radio is retuned, see
// dsp/if_filter.hpp). The reader is STRICT: a part it does not know, a number that is not a number, one band edge without the other, too many notches, an edge or a width outside what the filter accepts is refused with the first
// thing wrong, so a document with a damaged setting says so rather than running something else.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsp/if_filter.hpp"

namespace cascade::core::patch {

struct IfNotch {
    double rfHz = 0.0;
    double widthHz = 0.0;
    bool operator==(const IfNotch&) const = default;
};

struct IfSettings {
    bool band = false;
    double lowHz = 0.0, highHz = 0.0;
    std::vector<IfNotch> notches;
    bool operator==(const IfSettings&) const = default;
};

inline bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}

inline bool parseIfSettings(const std::string& text, IfSettings& out, std::string& why) {
    IfSettings s;
    bool haveLow = false, haveHigh = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t semi = text.find(';', at);
        if (semi == std::string::npos) { semi = text.size(); }
        const std::string part = text.substr(at, semi - at);
        at = semi + 1;
        if (part.empty()) { continue; }
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) { why = "an IF filter setting is name=value"; return false; }
        const std::string name = part.substr(0, eq), value = part.substr(eq + 1);
        if (name == "low" || name == "high") {
            double v = 0.0;
            if (!parseNumber(value, v) || v < 0.0 || v > 1.0e12) { why = name + " is a frequency in hertz"; return false; }
            if (name == "low") { s.lowHz = v; haveLow = true; } else { s.highHz = v; haveHigh = true; }
        } else if (name == "notch") {
            std::size_t p = 0;
            while (p <= value.size()) {
                std::size_t comma = value.find(',', p);
                if (comma == std::string::npos) { comma = value.size(); }
                const std::string one = value.substr(p, comma - p);
                p = comma + 1;
                if (one.empty()) { if (comma >= value.size()) { break; } continue; }
                const std::size_t slash = one.find('/');
                IfNotch n;
                if (slash == std::string::npos || !parseNumber(one.substr(0, slash), n.rfHz) || !parseNumber(one.substr(slash + 1), n.widthHz) || n.rfHz < 0.0 || n.rfHz > 1.0e12 ||
                    n.widthHz < dsp::IfFilter::kMinNotchWidthHz || n.widthHz > 1.0e7) {
                    why = "a notch is <frequency Hz>/<width Hz>, a width of at least 5 Hz";
                    return false;
                }
                if (static_cast<int>(s.notches.size()) >= dsp::IfFilter::kMaxNotches) { why = "an IF filter has at most 8 notches"; return false; }
                s.notches.push_back(n);
                if (comma >= value.size()) { break; }
            }
        } else {
            why = "the IF filter settings are low, high and notch";
            return false;
        }
    }
    if (haveLow != haveHigh) { why = "the pass-band needs both low and high"; return false; }
    if (haveLow) {
        if (s.highHz - s.lowHz < dsp::IfFilter::kMinBandwidthHz) { why = "the pass-band must be at least 10 Hz wide"; return false; }
        s.band = true;
    }
    out = s;
    return true;
}

// MIXER (NodeKind::Mixer): "gain=<g0>,<g1>,<g2>,<g3>;mute=<m0>,<m1>,<m2>,<m3>" in either order, both optional. A gain is a linear factor 0 to 4 (1 = as it is; fewer numbers than inputs leave the rest at 1), a mute is 0 or 1.
// A mixer is a plain sum of the inputs' audio, each times its gain and not at all when muted (the stereo AudioMixer model, with balance, solo and a limiter, is not used yet). Empty = every input at 1.
struct MixerSettings {
    float gain[kMixerInputs] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool mute[kMixerInputs] = {false, false, false, false};
    bool operator==(const MixerSettings& o) const {
        for (std::size_t i = 0; i < kMixerInputs; ++i) { if (gain[i] != o.gain[i] || mute[i] != o.mute[i]) { return false; } }
        return true;
    }
};

inline bool parseMixerSettings(const std::string& text, MixerSettings& out, std::string& why) {
    MixerSettings m;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t semi = text.find(';', at);
        if (semi == std::string::npos) { semi = text.size(); }
        const std::string part = text.substr(at, semi - at);
        at = semi + 1;
        if (part.empty()) { continue; }
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) { why = "a mixer setting is name=values"; return false; }
        const std::string name = part.substr(0, eq), values = part.substr(eq + 1);
        if (name != "gain" && name != "mute") { why = "the mixer settings are gain and mute"; return false; }
        std::size_t i = 0, p = 0;
        while (p <= values.size() && !values.empty()) {
            std::size_t comma = values.find(',', p);
            if (comma == std::string::npos) { comma = values.size(); }
            double v = 0.0;
            if (i >= kMixerInputs || !parseNumber(values.substr(p, comma - p), v)) { why = "a mixer has four inputs, each setting a number"; return false; }
            if (name == "gain") {
                if (v < 0.0 || v > 4.0) { why = "a mixer gain is 0 to 4"; return false; }
                m.gain[i] = static_cast<float>(v);
            } else {
                if (v != 0.0 && v != 1.0) { why = "a mixer mute is 0 or 1"; return false; }
                m.mute[i] = v == 1.0;
            }
            ++i;
            p = comma + 1;
            if (comma >= values.size()) { break; }
        }
    }
    out = m;
    return true;
}

// Configures a filter at sample rate and centre from the settings (all off when `s` has nothing).
inline void applyIfSettings(dsp::IfFilter& f, const IfSettings& s, double centreRfHz) {
    f.setCentre(centreRfHz);
    if (s.band) { f.setBand(s.lowHz, s.highHz, true); }
    for (std::size_t i = 0; i < s.notches.size(); ++i) { f.setNotch(static_cast<int>(i), s.notches[i].rfHz, s.notches[i].widthHz, true); }
}

}  // namespace cascade::core::patch
