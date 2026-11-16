// lanes2_logic.hpp - SCREEN LANE S2 (docs/multi-radio-records/screen-s2.md): the pure rules behind the spectrum and tuning controls lane S2 adds to the Slint window - no Slint, no Win32, no engine call.
//
//   G2-01/02/03  the spectrum's FFT size, window and frame rate       (SET_SPECTRUM)
//   G2-12        the IF-passband zoom, its picture and its readout      (SET_IF_ZOOM, Engine::zoomFrame)
//   G2-09        the waterfall's autosave: when a picture is due, and the file it is written to (the recorder's name rules)
//   G2-16        the power and SNR log: the CSV line values from one spectrum frame
//   G2-32        band stacking: the bands and their registers over core/operating_aids.hpp BandStacking
//   G2-33        VFO A / B read-outs
//
// The typed-field helpers (parseNumber, parseInteger, trim, numberText, Built) are lane L's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slints2 {

// ============================================================ FFT size, window, frame rate (G2-01, G2-02, G2-03) ============================================================
constexpr int kFftSizeCount = 8;
std::uint32_t fftSizeAt(int index);                 // 1024, 2048 ... 131072 (clamped)
int fftIndexOf(std::uint32_t size);                 // -1 when the size is not one of the eight
std::vector<std::string> fftSizeLabels();           // "1024" ... "131072", as the digits the person reads
constexpr int kWindowCount = 5;
const char* windowName(int index);                  // English: "Rectangular", "Hann", "Hamming", "Blackman-Harris", "Flat top" (the host translates the label it shows)
// The frame rate typed: 0 or the word "auto"/"automatic" = automatic; else 1 to 120 a second (the engine's own range).
bool parseFrameRate(const std::string& text, double& out, std::string& err);
std::string frameRateText(double hz);               // "0" for automatic (the field's value), else the number
// The numbers of the engine's operation: ival[0] size, ival[1] window, num[0] rate. One struct so a change of one of the three always sends the other two as they are now.
struct SpectrumSetting { std::uint32_t size = 1024; int window = 3; double fps = 0.0; };
// How many bins one frame of this size carries and how often: "1024 points, about 30 a second" - the figure under the control.
std::string spectrumFigure(const SpectrumSetting& s, const std::string& autoWord, const std::string& perSecondWord);

// ============================================================ IF zoom (G2-12) ============================================================
constexpr int kZoomSizeCount = 8;                   // off, 256, 512 ... 16384
std::uint32_t zoomSizeAt(int index);                // 0 for index 0 (off)
int zoomIndexOf(std::uint32_t size);                // -1 when it is none of them
std::vector<std::string> zoomSizeLabels(const std::string& offWord);
// The width of the picture in Hz: a number from 100 up to 1e7 (the engine refuses a width above the channel rate, in its own words).
bool parseZoomSpan(const std::string& text, double& out, std::string& err);
// The fact fact.radio.<n>.zoom: "off", or "<bins> bins of <binHz> Hz over <span> Hz".
struct ZoomFact { bool on = false; std::uint32_t bins = 0; double binHz = 0.0, spanHz = 0.0; };
bool parseZoomFact(const std::string& fact, ZoomFact& out);   // false for an empty or unreadable fact; "off" gives on = false
// The zoom picture: `w` x `h` RGBA bytes (a dark ground, a graticule, the trace in a light colour with a faint fill), the levels in `bins` (dB, FFT-shifted: the middle bin is the passband's centre),
// mapped from dbMin at the bottom to dbMax at the top; the clean 80 % (usableFrac) is drawn at full strength, the two edge strips dimmed. The mapping of a column to a bin takes the strongest bin of the
// group the column covers, so a narrow carrier is never lost to the width of the picture. Empty `bins` gives the ground only.
std::vector<std::uint8_t> paintZoom(std::uint32_t w, std::uint32_t h, const float* bins, std::size_t n, double dbMin, double dbMax, double usableFrac);
// The tallest column of the trace (0 at the top ... h at the bottom): for a test to find the carrier.
std::uint32_t paintedPeakColumn(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h);

// ============================================================ waterfall autosave (G2-09) ============================================================
constexpr int kAutosaveChoices = 5;                 // 1, 5, 15, 30, 60 minutes
int autosaveMinutes(int index);                     // clamped
int autosaveIndexOf(int minutes);                   // -1 when none
// True when a picture is due: `lastSavedMs` is when the last was started (0 = never since it was switched on), `nowMs` the steady clock, both in ms.
bool autosaveDue(std::int64_t lastSavedMs, std::int64_t nowMs, int minutes);
// The folder the person typed made usable: quotes and spaces off, "" when nothing usable or when it is a relative path (an autosave has no base to be relative to), the trailing separator off.
std::string cleanFolder(const std::string& typed);
// The path (without extension) of the next picture below `folder`: the recorder's file-name template (fact.record.nameTemplate, core/rec_naming.hpp) filled in from the moment, the radio's tune and
// mode, or "waterfall_{datetime}" when the template is empty or refused. Returns "" when no usable path comes out. `exists` lets a test and the host say which names are taken: the first free
// "name", "name (2)" ... is returned.
struct AutosaveFields { std::int64_t unixMs = 0; int utcOffsetMinutes = 0; double frequencyHz = 0.0; std::string mode; int radio = 1; };
std::string autosavePath(const std::string& folder, const std::string& recorderTemplate, const AutosaveFields& f, std::uint64_t counter);

// ============================================================ the power and SNR log (G2-16) ============================================================
// The interval typed: seconds, 0.1 to 3600.
bool parseLogInterval(const std::string& text, double& out, std::string& err);
// The values of one log line, from one spectrum frame and the passband in bins [p0, p1): channel power (the passband's bins summed), the floor (the median outside it), the SNR (peak over floor).
struct PowerReading { bool valid = false; double signalDbfs = 0, noiseDbfs = 0, snrDb = 0; };
PowerReading readPower(const float* bins, std::size_t n, std::size_t p0, std::size_t p1);

// ============================================================ band stacking (G2-32) ============================================================
// The bands the keys offer, in the order the person reads them. Names are radio vocabulary (not translated).
struct BandInfo { const char* name; double lowHz, highHz; const char* mode; double bandwidthHz; double centreHz; };
int bandCount();
const BandInfo& bandAt(int index);
// The state text kept per register for the saved file and the readout: "7.074000 MHz USB 2.7 kHz".
std::string registerText(double hz, const std::string& mode, double bandwidthHz);

// ============================================================ VFO A / B (G2-33) ============================================================
// "A" or "B".
const char* vfoLetter(std::uint32_t active);
// "7.074000 MHz  USB  2.7 kHz"; "not set" words come from the caller when the register has never been set (hz == 0).
std::string vfoText(double hz, const std::string& mode, double bandwidthHz);

}  // namespace foxsdr::slints2
