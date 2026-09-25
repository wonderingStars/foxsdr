// freq_converter.hpp - an up-converter or down-converter in front of the
// radio, and THE ONE PLACE the arithmetic between the two frequencies lives.
//
// WHY THIS EXISTS. A beta tester listens to SAQ Grimeton on 17.2 kHz and to
// the VLF naval stations around it. No radio he owns tunes that low, so the
// antenna goes through a home-built up-converter first: a mixer and a local
// oscillator (LO) that lift everything by the LO's frequency - 2 MHz,
// 100 MHz, or the 125 MHz of the common Ham-It-Up converters. The radio then
// sits at 125.0172 MHz to hear 17.2 kHz. The same holds the other way round
// for a down-converter (a satellite LNB brings 10.489 GHz down to 739 MHz
// with a 9.75 GHz LO).
//
// TWO FREQUENCIES, AND ONLY ONE OF THEM IS SHOWN. The AIR frequency is where
// the signal really is - 17.2 kHz - and it is the only one the user, the
// counter, the spectrum axis, the band plan, the bookmarks, the plugins, the
// web remote and CAT ever see. The RADIO frequency is what the radio is told,
// and nothing outside the translation layer (core::Pipeline's source view
// and core::patch::PatchRadio) speaks it. Every function below is pure, so
// the whole rule is testable without a radio (tests/test_freq_converter.cpp).
//
// THE RULE, for a converter with local oscillator LO:
//   Off                          radio = air
//   Up-converter                 radio = air + LO     (125 MHz: 17.2 kHz -> 125.0172 MHz)
//   Down-converter               radio = air - LO     (LNB: 10.489 GHz -> 739 MHz)
//   either, spectrum INVERTED    radio = LO - air     (the mixer's other product: a
//                                                      high-side LO, which also turns
//                                                      the spectrum back to front)
// and air = the same rule solved the other way. An inverted converter also
// MIRRORS the band: a signal 1 kHz above the air frequency reaches the radio
// 1 kHz BELOW its centre. The source views conjugate the I/Q samples for that
// (conjugateInPlace), which puts the band back the right way round, so the
// VFO offset, the spectrum and every decoder keep meaning air.
//
// NEVER APPLIED BY DEFAULT. A converter is remembered PER RADIO (the map in
// AppConfig::converters, keyed by converterRadioKey), is off for any radio
// the user has not set one for, and is reset to off by every source install
// (Pipeline::setSource) until the application applies the new radio's own
// setting. The signal generator and an I/Q file have keys of their own, so
// a converter reaches either only when the user sets it there explicitly.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>

#include "core/patch_devices.hpp"

namespace cascade::core {

enum class ConverterMode : std::uint8_t { Off, Up, Down };

struct ConverterSetting {
    ConverterMode mode = ConverterMode::Off;
    double loHz = 0.0;       // the converter's local oscillator
    bool inverted = false;   // radio = LO - air, spectrum mirrored
    bool operator==(const ConverterSetting& o) const {
        return mode == o.mode && loHz == o.loHz && inverted == o.inverted;
    }
    bool operator!=(const ConverterSetting& o) const { return !(*this == o); }
};

// A local oscillator is 1 Hz or more (a typed 0 is "not set yet") and at most
// 300 GHz, which is past every converter anyone builds for a receiver and far
// short of a number that stops being a frequency.
inline constexpr double kConverterMinLoHz = 1.0;
inline constexpr double kConverterMaxLoHz = 300.0e9;
// A config listing more radios than this is a hand-edit: nobody owns that many.
inline constexpr std::size_t kMaxConverterRadios = 64;

inline bool converterLoValid(double loHz) {
    return std::isfinite(loHz) && loHz >= kConverterMinLoHz && loHz <= kConverterMaxLoHz;
}

// On, with an LO that means something. A mode with no valid LO is OFF: a
// half-typed setting must never retune a radio by an arbitrary amount.
inline bool converterActive(const ConverterSetting& s) {
    return s.mode != ConverterMode::Off && converterLoValid(s.loHz);
}

// Whether the source view must mirror the band (conjugate the samples).
inline bool converterMirrors(const ConverterSetting& s) {
    return converterActive(s) && s.inverted;
}

// AIR -> RADIO: what the radio must be told to hear `airHz`. May be zero or
// negative, which is a frequency no radio can tune: airReachable says so.
inline double radioFromAir(const ConverterSetting& s, double airHz) {
    if (!converterActive(s)) { return airHz; }
    if (s.inverted) { return s.loHz - airHz; }
    return s.mode == ConverterMode::Up ? airHz + s.loHz : airHz - s.loHz;
}

// RADIO -> AIR: what a radio sitting at `radioHz` is hearing on the air.
inline double airFromRadio(const ConverterSetting& s, double radioHz) {
    if (!converterActive(s)) { return radioHz; }
    if (s.inverted) { return s.loHz - radioHz; }
    return s.mode == ConverterMode::Up ? radioHz - s.loHz : radioHz + s.loHz;
}

// Whether the converter can deliver `airHz` to the radio at all - i.e. the
// radio frequency it maps to is above 0 Hz. With no converter this has no
// opinion (true): the radio's own range, checked by its driver, is the only
// limit then.
//
// A NEGATIVE AIR CENTRE IS REACHABLE. The source centre is the tuned station
// minus the VFO offset, so a 16.4 kHz CW preset with the VFO parked 300 kHz up
// puts the centre at -283.6 kHz on the air - which through a 125 MHz
// up-converter is 124.7164 MHz at the radio, an ordinary tune, with the
// station inside the band. Found by the first scripted run of this feature,
// which refused exactly that preset.
inline bool airReachable(const ConverterSetting& s, double airHz) {
    if (!converterActive(s)) { return true; }
    return std::isfinite(airHz) && radioFromAir(s, airHz) > 0.0;
}

// WHETHER A CENTRE TYPED FOR A RADIO CAN BE TAKEN: the radio frequency it
// maps to must be above 0 Hz, converter or not. Unlike airReachable this has
// an opinion with no converter (air IS radio then, so above 0 Hz only), and
// through an up-converter it takes an air centre of 0 Hz or below - -283.6 kHz
// behind a 125 MHz up-converter is 124.7164 MHz at the radio. Used by the
// patch page's Radio node, whose centre may be carried in negative.
inline bool radioCentreTakeable(const ConverterSetting& s, double airHz) {
    return std::isfinite(airHz) && radioFromAir(s, airHz) > 0.0;
}

// The lowest TUNED air frequency (source centre + VFO offset) a tuning
// control - the counter's wheel and switches, the tuning keys - may step
// down to. With no converter it is the rule those controls always had: never
// below 0 Hz, and never below the offset (the source centre is never asked
// for a negative frequency). Through a converter the centre may go negative
// on the air (see airReachable); what must stay above 0 Hz is the RADIO, so
// the floor is where the centre reaches the converter's own limit - below the
// LO for an up-converter, the LO itself for a down-converter - and never below
// 0 Hz tuned. An inverting converter's limit is at the TOP (air below LO), so
// its floor is 0 Hz; a tune past its top is refused and said so.
inline double minTunedAirHz(const ConverterSetting& s, double vfoOffsetHz) {
    if (!converterActive(s)) { return std::max(0.0, vfoOffsetHz); }
    if (s.inverted) { return 0.0; }
    const double lowestCentre = s.mode == ConverterMode::Up ? -s.loHz : s.loHz;
    return std::max(0.0, lowestCentre + vfoOffsetHz);
}

// THE AIR RANGE a radio covers through the converter. `hasRadioRange` false
// means the radio published none, which is read as "anything above 0 Hz" so
// the converter's own limit can still be stated. The answer is clipped at
// 0 Hz (there is no negative air frequency) and may run to +infinity (an
// up-converter in front of a radio with no published top). False when the
// radio's range maps to nothing at or above 0 Hz. With no converter the
// radio's own range is handed back unchanged, and false when it has none.
inline bool airRangeHz(const ConverterSetting& s, bool hasRadioRange, double radioLoHz,
                       double radioHiHz, double& airLoHz, double& airHiHz) {
    if (!converterActive(s)) {
        if (!hasRadioRange) { return false; }
        airLoHz = radioLoHz;
        airHiHz = radioHiHz;
        return radioHiHz > radioLoHz;
    }
    const double inf = std::numeric_limits<double>::infinity();
    const double rLo = hasRadioRange ? std::max(0.0, radioLoHz) : 0.0;
    const double rHi = hasRadioRange ? radioHiHz : inf;
    if (!(rHi > rLo)) { return false; }
    const double a = airFromRadio(s, rLo);
    const double b = airFromRadio(s, rHi);
    double lo = std::min(a, b);
    const double hi = std::max(a, b);
    lo = std::max(lo, 0.0);
    if (!(hi > lo)) { return false; }
    airLoHz = lo;
    airHiHz = hi;
    return true;
}

// The mirror, on the samples: a signal at +f becomes one at -f. Applied by the
// source views when converterMirrors() says the converter inverts.
inline void conjugateInPlace(std::complex<float>* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { s[i] = std::conj(s[i]); }
}

// --- Persistence -------------------------------------------------------------

// The config file's words for the three modes. Anything else reads as OFF: a
// value this build does not understand must never shift a radio's tuning.
inline const char* converterModeKey(ConverterMode m) {
    switch (m) {
        case ConverterMode::Up: return "up";
        case ConverterMode::Down: return "down";
        case ConverterMode::Off: break;
    }
    return "off";
}

inline ConverterMode converterModeFromKey(const std::string& key) {
    if (key == "up") { return ConverterMode::Up; }
    if (key == "down") { return ConverterMode::Down; }
    return ConverterMode::Off;
}

// WHICH RADIO a setting belongs to. The same grammar the patch page uses for
// its radios (core/patch_devices.hpp), so a converter set on the receiver's
// dongle is the one a patch Radio node on that dongle uses too:
//   "siggen"          the signal generator
//   "file"            I/Q file playback (every file: a recording made THROUGH
//                     a converter already carries air frequencies, so this is
//                     for a file somebody else recorded at the radio's)
//   "<kind>|<args>"   a radio: the driver key and the args it was opened with
inline std::string converterRadioKey(const std::string& kind, const std::string& args) {
    if (kind == "siggen" || kind.empty()) { return patch::kGeneratorKey; }
    if (kind == "file") { return "file"; }
    return patch::makeDeviceKey(kind, args);
}

// A setting read from a file, made safe: an invalid LO turns the converter
// OFF (and forgets the LO) rather than keeping a mode that cannot be applied.
//
// AND THE LO IS WHOLE HERTZ. A radio keeps what it is told in whole hertz (an
// RTL-SDR's tuner call takes a uint32), so a fractional LO would tell it a
// fractional frequency, lose the fraction, and read every tune back as an air
// frequency a fraction of a hertz from the one asked for - the counter
// disagreeing with itself, and the repeat-tune guard (applyRetuneNow's "no-op
// when the tune moves nothing") never matching. Rounded here, where every
// loaded and every changed setting passes, and in parseConverterLoHz.
inline ConverterSetting sanitiseConverter(ConverterSetting s) {
    if (std::isfinite(s.loHz)) { s.loHz = std::round(s.loHz); }
    if (!converterLoValid(s.loHz)) {
        s.mode = ConverterMode::Off;
        s.loHz = 0.0;
    }
    return s;
}

// The whole remembered map, made safe: empty keys dropped, every setting
// sanitised, and no more than kMaxConverterRadios kept.
inline std::map<std::string, ConverterSetting> sanitiseConverters(
    const std::map<std::string, ConverterSetting>& in) {
    std::map<std::string, ConverterSetting> out;
    for (const auto& [key, s] : in) {
        if (key.empty()) { continue; }
        if (out.size() >= kMaxConverterRadios) { break; }
        out[key] = sanitiseConverter(s);
    }
    return out;
}

// The setting for one radio: what was remembered, or OFF.
inline ConverterSetting converterFor(const std::map<std::string, ConverterSetting>& all,
                                     const std::string& radioKey) {
    const auto it = all.find(radioKey);
    return it == all.end() ? ConverterSetting{} : sanitiseConverter(it->second);
}

// --- Text ----------------------------------------------------------------------

// A frequency as a person writes it, with no trailing zeros: "125 MHz",
// "2 MHz", "16.4 kHz", "125.0164 MHz", "9.75 GHz", "0 Hz". Resolution is
// 1 Hz, which is finer than any converter's LO is known to. Units are not
// translated (i18n.hpp: Hz is universal vocabulary).
inline std::string converterHzText(double hz) {
    if (!std::isfinite(hz)) { return hz > 0 ? "inf" : "-inf"; }
    const double a = std::fabs(hz);
    const char* unit = "Hz";
    double div = 1.0;
    if (a >= 1.0e9) {
        unit = "GHz";
        div = 1.0e9;
    } else if (a >= 1.0e6) {
        unit = "MHz";
        div = 1.0e6;
    } else if (a >= 1.0e3) {
        unit = "kHz";
        div = 1.0e3;
    }
    // Round to whole hertz first, then print with as many decimals as the
    // unit needs to show them (9 for GHz, 6 for MHz, 3 for kHz) and trim.
    const double wholeHz = std::round(hz);
    const int decimals = div >= 1.0e9 ? 9 : div >= 1.0e6 ? 6 : div >= 1.0e3 ? 3 : 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, wholeHz / div);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
    }
    if (s == "-0") { s = "0"; }
    return s + " " + unit;
}

// A typed local oscillator: "125", "125 MHz", "125M", "100000 kHz", "2e6 Hz",
// "9.75 GHz", "125,000,000". Spaces, commas and underscores are ignored and a
// Hz/kHz/MHz/GHz suffix (any case, "Hz" optional after the prefix) wins. A
// BARE number is MHz when it is at most 7500 - the same rule the frequency
// counter's typed entry uses - and hertz above that, so "125" and
// "125000000" both mean 125 MHz. False on anything else, or on an LO outside
// what converterLoValid accepts; `outHz` is untouched then.
inline bool parseConverterLoHz(const std::string& text, double& outHz) {
    std::string clean;
    for (const char c : text) {
        if (c == ' ' || c == ',' || c == '_' || c == '\'') { continue; }
        clean.push_back(c);
    }
    if (clean.empty() || clean.size() > 40) { return false; }
    const char* begin = clean.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value) || value < 0.0) { return false; }
    std::string rest(end);
    for (char& c : rest) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    double scale = 0.0;
    if (rest.empty()) {
        scale = value <= 7500.0 ? 1.0e6 : 1.0;
    } else if (rest == "hz") {
        scale = 1.0;
    } else if (rest == "k" || rest == "khz") {
        scale = 1.0e3;
    } else if (rest == "m" || rest == "mhz") {
        scale = 1.0e6;
    } else if (rest == "g" || rest == "ghz") {
        scale = 1.0e9;
    } else {
        return false;
    }
    // WHOLE HERTZ, like every LO sanitiseConverter lets through: see there.
    const double hz = std::round(value * scale);
    if (!converterLoValid(hz)) { return false; }
    outHz = hz;
    return true;
}

}  // namespace cascade::core
