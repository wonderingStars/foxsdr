// ppm_correction.hpp - the crystal correction ("PPM frequency correction",
// 0.99.56) and THE ONE PLACE its arithmetic lives.
//
// WHY THIS EXISTS. Every radio derives its tuning from a crystal, and no two
// crystals run at exactly the frequency printed on them. The error is a fixed
// PROPORTION - a few parts per million on a good radio, tens on a cheap
// dongle - so it grows with frequency: 5 ppm is 500 Hz at 100 MHz and 8.6 kHz
// at 1.7 GHz, enough to put a narrow signal outside its filter. The owner asked
// for "a PPM frequency correction function that you can toggle on in settings".
//
// THE SIGN, and it is the one every SDR program uses (librtlsdr's
// rtlsdr_set_freq_correction, rtl_sdr -p, SoapySDR's setFrequencyCorrection):
// a POSITIVE correction means the radio's crystal runs FAST, so a radio told X
// really sits at X * (1 + ppm/1e6), and a station of known frequency shows up
// BELOW where it should. Correcting it means asking for less.
//
// THREE WAYS TO APPLY IT (ppmMethodFor), best first:
//   InRadio        the radio corrects itself - the native RTL-SDR driver
//                  (which trims the resampler and the tuner's reference, so the
//                  SAMPLE RATE is corrected too) and SoapySDR devices whose
//                  driver reports hasFrequencyCorrection.
//   Retune         everything else: the radio is asked for f / (1 + ppm/1e6)
//                  so that it really lands on f (ppmRequestHz), and what it
//                  reports is read back the other way (ppmReadbackHz). The
//                  CENTRE frequency only - the sample rate keeps its error.
//                  Applied in the source view (source/converter_view.hpp),
//                  which both translation layers own: Pipeline::activeSource()
//                  for the receiver and core::patch::PatchRadio for the patch
//                  page's radios. Everything the user sees - the counter, the
//                  spectrum axis, bookmarks, decoders, the patch - stays in
//                  TRUE frequency, exactly as with a converter.
//   NotApplicable  the signal generator, a sound card and an I/Q file: none
//                  has a crystal of its own being corrected here.
//
// OFF IS BIT-FOR-BIT TODAY'S BEHAVIOUR. The switch is global and defaults off;
// the VALUE is kept per radio (a crystal's error belongs to one radio). With the
// switch off - or a radio with no value - every function below returns its
// input unchanged (a 0 correction is an identity, not a multiply by 1.0), and no
// radio is sent a correction at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "source/device_source.hpp"

namespace cascade::core {

// The range a user may type. Past +/-200 ppm is not a crystal error any radio
// ships with; it is a typo, and it would move a 1.7 GHz tune by 340 kHz.
inline constexpr double kPpmMin = -200.0;
inline constexpr double kPpmMax = 200.0;
// One decimal place: the finest step any calibration method a user has (a
// known broadcast carrier, a GSM cell, a GPS-locked reference) resolves.
inline constexpr double kPpmStep = 0.1;
// A config listing more radios than this is a hand-edit.
inline constexpr std::size_t kMaxPpmRadios = 64;

// A value made safe: not finite -> 0, clamped to the range, rounded to the
// 0.1 ppm step. Never a negative zero (it would print "-0.0").
inline double sanitisePpm(double ppm) {
    if (!std::isfinite(ppm)) { return 0.0; }
    ppm = std::clamp(ppm, kPpmMin, kPpmMax);
    const double r = std::round(ppm * 10.0) / 10.0;
    return r == 0.0 ? 0.0 : r;
}

// The value as the UI and the log print it: "+1.5", "-12.0", "+0.0".
inline std::string ppmText(double ppm) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%+.1f", sanitisePpm(ppm));
    return buf;
}

// Whether a source of this kind has a crystal the correction is for. The
// patch page's I/Q recordings are "iqfile"; the receiver's are "file".
inline bool ppmKindApplies(const std::string& kind) {
    return !(kind.empty() || kind == "siggen" || kind == "file" || kind == "iqfile" ||
             kind == "soundcard");
}

// WHICH RADIO a value belongs to: the driver family and the serial, the same
// identity the bias tee memory uses (core::biasTeeRadioKey) - a place in a USB
// walk ("index=0") is not a radio. ONE CRYSTAL, ONE KEY, WHICHEVER DRIVER
// OPENED IT: a dongle opened through SoapySDR ("driver=rtlsdr, serial=X") is
// keyed by the driver its args name, so it finds the value set when the native
// driver opened it ("rtlsdr|serial=X") - which is also what makes the value
// follow a dongle the native driver refused onto the SoapySDR fallback. A
// radio with no serial is keyed by its kind and whole args. "" for a kind the
// correction does not apply to.
inline std::string ppmRadioKey(const std::string& kind, const std::string& args) {
    if (!ppmKindApplies(kind)) { return {}; }
    std::string family = kind;
    if (kind == "soapy") {
        std::string driver = cascade::source::argValue(args, "driver");
        for (char& c : driver) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        if (!driver.empty()) { family = driver; }
    }
    const std::string serial = cascade::source::argValue(args, "serial");
    if (!serial.empty()) { return family + "|serial=" + serial; }
    return kind + "|" + args;
}

enum class PpmMethod : std::uint8_t { NotApplicable, InRadio, Retune };

// How a correction is applied to a source of `kind` whose driver does
// (`radioCorrects`) or does not correct its own crystal. SDRplay has a
// correction of its own in its API but FoxSDR does not drive it yet, so it
// reports none and is corrected by retuning.
inline PpmMethod ppmMethodFor(const std::string& kind, bool radioCorrects) {
    if (!ppmKindApplies(kind)) { return PpmMethod::NotApplicable; }
    return radioCorrects ? PpmMethod::InRadio : PpmMethod::Retune;
}

// Whether the radio's own correction takes WHOLE ppm only: the native RTL-SDR
// driver's register (Rtl2832u::setFreqCorrectionPpm takes an int, as
// librtlsdr's does). The typed value is rounded to the nearest whole ppm,
// halves away from zero (ppmWholeForRadio); the UI says which number went in.
inline bool ppmRadioTakesWholePpm(const std::string& kind) { return kind == "rtlsdr"; }
inline int ppmWholeForRadio(double ppm) {
    return static_cast<int>(std::lround(sanitisePpm(ppm)));
}

// The correction in force for the radio `key`: its remembered value while the
// switch is on, 0 otherwise (switch off, no value, or a key of "" - a source
// the correction does not apply to).
inline double ppmFor(bool enabled, const std::map<std::string, double>& values,
                     const std::string& key) {
    if (!enabled || key.empty()) { return 0.0; }
    const auto it = values.find(key);
    return it == values.end() ? 0.0 : sanitisePpm(it->second);
}

// --- the retune fallback, both directions -------------------------------------

// TRUE -> RADIO: what a radio `ppm` out must be told to land on `trueHz`.
// WHOLE HERTZ, for the reason core::sanitiseConverter gives: a radio keeps what
// it is told in whole hertz (an RTL-SDR's tuner call takes a uint32), so a
// fraction would be lost there and every readback would disagree with the
// request. The half-hertz this rounds away is 0.005 ppm at 100 MHz. A zero
// correction is the identity - not even the rounding is applied.
inline double ppmRequestHz(double trueHz, double ppm) {
    if (ppm == 0.0) { return trueHz; }
    return std::round(trueHz / (1.0 + ppm * 1.0e-6));
}

// RADIO -> TRUE: where a radio `ppm` out that reports `radioHz` really is.
inline double ppmTrueHz(double radioHz, double ppm) {
    if (ppm == 0.0) { return radioHz; }
    return radioHz * (1.0 + ppm * 1.0e-6);
}

// THE LAST TUNE, remembered so a readback can be answered EXACTLY. ppmTrueHz
// of a rounded request is the requested frequency to within half a hertz, not
// equal to it - and a counter reading 100.000000 MHz as 99.9999996 MHz, or the
// repeat-tune guard (AppWindow::applyRetuneNow, "no-op when the tune moves
// nothing") never matching, is the disagreement core::sanitiseConverter exists
// to prevent. So: when the radio still reports exactly what it was told, it is
// exactly where the user asked.
struct PpmMemo {
    bool valid = false;
    double askedHz = 0.0;   // the TRUE frequency the tune was for
    double sentHz = 0.0;    // what the radio was told (ppmRequestHz of it)
};

// RADIO -> TRUE, as the source view reads it back. A radio that answered
// somewhere other than where it was told (an edge clamp, its own tuning step)
// is read through ppmTrueHz, so a coerced tune still shows up as one.
inline double ppmReadbackHz(double radioHz, double ppm, const PpmMemo& memo) {
    if (ppm == 0.0) { return radioHz; }
    if (memo.valid && radioHz == memo.sentHz) { return memo.askedHz; }
    return ppmTrueHz(radioHz, ppm);
}

// --- persistence ---------------------------------------------------------------

// The remembered map made safe: empty keys dropped, every value sanitised, a
// value of 0 dropped (0 is what a missing entry means anyway), at most
// kMaxPpmRadios kept.
inline std::map<std::string, double> sanitisePpmValues(const std::map<std::string, double>& in) {
    std::map<std::string, double> out;
    for (const auto& [key, v] : in) {
        if (key.empty()) { continue; }
        const double s = sanitisePpm(v);
        if (s == 0.0) { continue; }
        if (out.size() >= kMaxPpmRadios) { break; }
        out[key] = s;
    }
    return out;
}

}  // namespace cascade::core
