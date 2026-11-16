// s1_logic.hpp - SCREEN LANE S1 (wave 8; docs/multi-radio-records/screen-s1-signal.md): the pure rules behind the receive controls the SIGNAL page gained, each of which reaches an engine command that
// had no control on screen: the meter's unit, CW pitch and peak filter, the passband's presets, FM widths and shape, the squelches (noise, CTCSS, DCS), AFC and CW auto-tune, the multiplex record, the
// audio equaliser, audio high-pass and low-pass, the noise-reduction method, the notch bank, stereo placement (pan and pseudo-stereo) and the I/Q correction.
//
// Nothing here knows Slint or the engine's threads: it turns what the engine's state says into the words and levels a row shows, and what a person typed into the numbers a command carries (or the sentence
// that says why not). Every range is the engine command's own (docs/API.md 6.19, engine.cpp applyCommand); the old window (Legacy) has none of these controls, so there is nothing of its to match.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "s1_meter.hpp"

namespace foxsdr::slints1 {

// ============================================================ ranges: the engine commands' own ============================================================
struct Range { double lo, hi; };
inline constexpr Range kCwPitch{300.0, 1200.0};          // FOXAPI_OP_SET_CW_PITCH
inline constexpr Range kCwPeakWidth{30.0, 500.0};        // FOXAPI_OP_SET_CW_PEAK
inline constexpr Range kCwPeakBoost{0.0, 24.0};
inline constexpr Range kNoiseSqThreshold{-90.0, 0.0};    // FOXAPI_OP_SET_NOISE_SQUELCH
inline constexpr Range kNoiseSqTail{0.0, 2000.0};
inline constexpr Range kAfcPullIn{50.0, 20000.0};        // FOXAPI_OP_SET_AFC
inline constexpr Range kEqDb{-12.0, 12.0};               // FOXAPI_OP_SET_EQ_BAND
inline constexpr Range kNotchHz{20.0, 21600.0};          // FOXAPI_OP_SET_NOTCH_SLOT
inline constexpr Range kNotchWidth{1.0, 2000.0};
inline constexpr int kNotchSlots = 10;
inline constexpr int kEqBands = 8;

// A slider's 0..1 against a range, and back (clamped; a value outside the range or not a number reads as the range's low end).
double fromLevel(const Range& r, double level);
float toLevel(const Range& r, double value);
// The same, rounded to a step (a CW pitch to a hertz, an equaliser gain to half a decibel).
double fromLevelStep(const Range& r, double level, double step);

// ============================================================ the meter ============================================================
// sUnitsText and meterReading are in s1_meter.hpp (the Radio section's model quotes them too).
std::vector<std::string> meterUnitNames();   // "Level in dB", "S units", "Level in dBm" (translated)

// ============================================================ passband ============================================================
// The widths a mode's preset list offers, in Hz, narrowest first. FM modes (NFM, WFM) are the engine's five FM widths (FOXAPP_OP_SET_FM_WIDTH); the others are widths for SET_BANDWIDTH, which the
// engine holds at 3 kHz or more and at 0.9 of the channel rate.
std::vector<double> widthPresets(unsigned demodMode);
bool isFmMode(unsigned demodMode);
// "12.5 kHz", "150 kHz", "3 kHz" (not translated: a figure and its unit).
std::string widthText(double hz);
// Which preset the engine's bandwidth is (within 1 Hz), or -1.
int presetIndexOf(const std::vector<double>& presets, double bandwidthHz);
std::vector<std::string> fmWidthNames();       // "Narrow 12.5 kHz", ... for FOXAPP_OP_SET_FM_WIDTH 0..4 (the word translated)
std::vector<std::string> shapeNames();         // "Default", "Sharp", "Medium", "Soft" for FOXAPI_OP_SET_FILTER_SHAPE 0..3 (translated)

// ============================================================ squelch and tone ============================================================
// The CTCSS tone typed: "" or "0" is any tone; otherwise one of the 50. The engine's rule (dsp/tone_squelch.hpp) is the check.
bool parseCtcss(const std::string& text, double& hz, std::string& err);
// The DCS code typed as people write it ("023" or "23"); "" or "0" is any code; otherwise one of the 83 standard ones.
bool parseDcs(const std::string& text, int& digits, std::string& err);
// The tone after / before this one in the 50-tone list (0 = any: the first / last tone), and the same for the 83 DCS codes.
double nextCtcss(double hz, int direction);
int nextDcs(int digits, int direction);
std::string dcsText(int digits);                 // "023", "754", "any" is the caller's
std::string ctcssText(double hz);                // "88.5"
std::vector<std::string> toneKindNames();        // "Off", "CTCSS", "DCS" (translated)
std::vector<std::string> polarityNames();        // "Any", "Normal", "Inverted" (translated)

// ============================================================ audio ============================================================
std::vector<std::string> eqBandNames();          // "Bass", "Middle", "Treble", "100 Hz", "300 Hz", "1 kHz", "2.5 kHz", "5 kHz" (the three words translated)
extern const int kHighPassHz[3];                 // 0, 100, 300
extern const int kLowPassHz[4];                  // 0, 2400, 3000, 6000
int indexOfHighPass(unsigned hz);                // -1 when it is none of them
int indexOfLowPass(unsigned hz);
std::vector<std::string> highPassNames();        // "High-pass off", "High-pass 100 Hz", "High-pass 300 Hz"
std::vector<std::string> lowPassNames();
std::vector<std::string> nrMethodNames();        // "Standard noise reduction", "Spectral noise reduction", "Line enhancer (CW)"
std::vector<std::string> placementNames();       // "Placement off", "Pan", "Pseudo-stereo", "Binaural"
// The slider's range, default and readout text for a placement mode (FOXAPI_OP_SET_STEREO_PLACE 1..3).
Range placementRange(int mode);
double placementDefault(int mode);
std::string placementText(int mode, double value);

// The slot typed (1 to 10) as the engine's 0 to 9; false with the sentence on a bad one.
bool parseNotchSlot(const std::string& text, int& slot0, std::string& err);
bool parseNotchHz(const std::string& text, double& hz, std::string& err);
bool parseNotchWidth(const std::string& text, double& hz, std::string& err);
// "1: 1000 Hz wide 50 Hz; 3: 440 Hz wide 20 Hz" for the notches that are on, "" for none.
std::string notchSummary(unsigned mask, const double* hz, const double* width);

// ============================================================ I/Q correction ============================================================
inline constexpr unsigned kIqDc = 1u, kIqBalance = 2u, kIqSwap = 4u;
std::string imageRejectionText(double db);      // "34.2 dB", "" while nothing is measured (the engine says 200)

// ============================================================ the passband's edges and the AM detector (the engine's facts) ============================================================
inline constexpr Range kCutHz{0.0, 24000.0};            // FOXAPP_OP_SET_PASSBAND: a low cut or a high cut
inline constexpr Range kShiftHz{-6000.0, 6000.0};       // and the shift
// fact.radio.<n>.passband: "<on>;<low>;<high>;<shift>;<active>"
struct PassbandFact { bool ok = false, on = false, active = false; double low = 300.0, high = 2700.0, shift = 0.0; };
PassbandFact parsePassbandFact(const std::string& text);
// The edges a mode starts from when the person has not set any: voice window in SSB, the pitch plus and minus 250 Hz in CW, 4.5 kHz each side of the carrier in AM.
void defaultEdges(unsigned demodMode, double cwPitchHz, double& low, double& high);
bool hasPassband(unsigned demodMode);                   // USB, LSB, CW and AM
// What the two cuts mean in the mode (SSB and CW: audio edges, the high above the low; AM: how far each side of the carrier). False with the sentence when they cannot be.
bool checkCuts(unsigned demodMode, double low, double high, std::string& err);
// fact.radio.<n>.amdet: "<kind>;<locked>;<offset Hz>;<carrier presence>"
struct AmDetFact { bool ok = false, locked = false; int kind = 0; double offsetHz = 0.0, presence = 0.0; };
AmDetFact parseAmDetFact(const std::string& text);
std::vector<std::string> amDetectorNames();             // "Envelope detector", "Synchronous detector, both sidebands", "... upper sideband", "... lower sideband" (translated)

// ============================================================ the engine's state by name (for the drive scripts' read-back) ============================================================
// "cwPitchHz", "eqDb.3", "notchBankHz.2" ...; false for a name that is none of them.
bool stateValue(const FoxReceiverState& s, const std::string& name, double& out);

}  // namespace foxsdr::slints1
