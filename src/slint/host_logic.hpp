// The Slint host's pure logic: everything the main screen shows is computed from the C API table's state by
// these functions, and every key the operator presses becomes one submit() through them. No Slint type and
// no engine header appears here - only foxsdr_api.h - so the host stays a table client and this file is
// tested without a window (tests/test_slint_host_logic.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace foxsdr::slinthost {

// ---- the counter's ten figures: GHz, MHz x3, kHz x3, Hz x3 ----
std::array<int, 10> digitsOf(double hz);                   // clamped to 0 .. 9 999 999 999
int leadingZeros(const std::array<int, 10>& digits);       // how many leading figures draw dim (at most 9)
double stepOfDigit(int index);                             // 0 = 1 GHz ... 9 = 1 Hz
const char* placeName(int index);                          // "GHz", "100 MHz", ... "1 Hz"; "" out of range

// ---- modes ----
const char* modeName(std::uint32_t demod);                 // FOXAPI_DEMOD_* -> "NFM" .. "RAW", else "?"
int modeIndex(std::uint32_t demod);                        // the strip's segment index 0..7, -1 unknown
std::uint32_t demodOfIndex(int index);                     // the segment's FOXAPI_DEMOD_*
// The tuning key's step (Ctrl+Up / Ctrl+Down, the "Tune step" picker's start value): the key table says "Tune up one step (10 kHz)" and the classic window steps the 10 kHz digit, so the Slint window
// starts at 10 kHz too (screen stage S6, B5: it started at 12.5 kHz, which the label never said).
constexpr double kDefaultTuneStepHz = 10000.0;
double modeStepHz(std::uint32_t demod);                    // the tuning step per mode (NFM 12.5 kHz, WFM 100 kHz, AM 9 kHz, else 1 kHz)

// ---- the four lists a radio has of its own (screen stage S6, B1) ----
// GAINS, ANTENNAS, SAMPLE_RATES and DECODERS differ from radio to radio; every other list is the engine's, one for all.
bool isPerRadioList(std::uint32_t list);
// One item of a list as RADIO `radio` (1-based) has it: the four per-radio lists are asked for that radio by read_radio_list (so the screen does not depend on where the session's focus is),
// anything else, an engine without FOXAPI_CAP_RADIOS and radio 0 by read_list (the focus). Same contract as read_list: the count, `out` filled below it, negative = refused.
std::int32_t readListFor(const FoxEngineApi* api, FoxSession* session, std::uint32_t radio, std::uint32_t list, std::uint32_t index, FoxListItem* out);

// ---- readouts ----
std::string fmtMHz(double hz);                             // "145.500000 MHz"
std::string fmtRate(double hz);                            // "2.400 MS/s", "--" when there is no rate
std::string fmtKHz(double hz);                             // "12.5 kHz"
std::string spanText(double hz);                           // "SPAN 2.400 MHz" (GHz / MHz / kHz / Hz by size)
std::string cursorText(double hz, double db);              // "145.212 MHz · -78.4 dB"
std::string axisUnit(double spanHz);                       // the frequency axis's unit word
// The typed editor's entry: "145.5", "145.5 MHz", "145500 kHz", "145500000", "7,074" -> Hz. A bare number under
// 100 000 is MHz, else Hz. False for anything that is not a frequency.
bool parseFrequencyHz(const std::string& text, double& hz);
// The click-to-tune snap: the nearest multiple of the mode's step (no snap when shift is held or step <= 0).
double snapToStep(double hz, double stepHz, bool shift);
// The zoom window: a new span about a fixed point (the pointer's fraction), kept inside the full span.
struct ViewWindow { double centreHz; double spanHz; };
ViewWindow zoomAbout(ViewWindow view, double fullCentreHz, double fullSpanHz, double factor, double aboutFrac, double minSpanHz);
ViewWindow panBy(ViewWindow view, double fullCentreHz, double fullSpanHz, double dFrac);

// ---- the strip's dials as sliders ----
double squelchNorm(double squelchDb);                      // -120..0 dB -> 0..1; the table's -200 (always open) sits at 0
double squelchDbFromNorm(double norm);
bool squelchAuto(double squelchDb);
double levelNorm(double audioLevelDb);                     // the VOLUME meter's -60 dB floor -> 0, 0 dBFS -> 1
std::string levelText(double audioLevelDb, bool haveAudio); // "-12.4 dB", "-60 dB" at or below the floor, "--" with no audio
std::string gainText(bool haveStage, bool agcOn, double value, const std::string& unit); // "N/A", "AUTO", "32 dB"

// ---- the spectrum's axes (positions as fractions 0..1 of the plot: Slint floats are 32-bit) ----
struct Tick {
    double frac;
    std::string label;
};
// Nice 1-2-5 steps about `targetPx` apart; labels in MHz with the decimals the step needs. Empty for a
// degenerate span or width.
std::vector<Tick> frequencyTicks(double centreHz, double spanHz, double widthPx, double targetPx = 80.0);
// A line every 10 dB from the ceiling down; labels at 20 dB multiples when the pitch is under 40 px.
std::vector<Tick> dbTicks(double dbMin, double dbMax, double heightPx);
double fracOf(double hz, double centreHz, double spanHz);
double hzAt(double frac, double centreHz, double spanHz);
// The waterfall's age strip: "now" at the top, then the smallest nice step (1, 2, 5, 10, 15, 30 s, 1, 2, 5, 10,
// 30, 60 min) whose labels sit at least `minPitchPx` apart over the history the picture holds.
std::vector<Tick> ageTicks(double historySeconds, double heightPx, double minPitchPx = 48.0);

// ---- paint only on a new frame ----
bool isNewFrame(std::uint64_t seq, std::uint64_t lastSeq);

// ---- the painters: they work on a caller-owned RGBA buffer, so nothing is allocated per frame ----
struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};
Rgba heat(float t);                                        // the strength key's ramp, t clamped to 0..1
enum class TraceMode { Normal, PeakHold, Average };
struct TraceState {
    std::vector<float> peak;                               // the highest level each column has reached
    std::vector<float> average;                            // the running power average per column
    float averageAlpha = 0.1f;
};
void resetTrace(TraceState& state);
// Clears the buffer to transparent and draws the live trace, and in PeakHold / Average the bold held trace
// over it. `bins` are dBFS, `dbMin..dbMax` the display range.
void paintTrace(Rgba* px, std::uint32_t width, std::uint32_t height, const float* bins, std::size_t binCount,
                double dbMin, double dbMax, TraceState& state, TraceMode mode, Rgba live, Rgba held, Rgba average);
// Scrolls the picture down one row and paints the frame along row 0 through heat().
void scrollWaterfallRow(Rgba* px, std::uint32_t width, std::uint32_t height, const float* bins, std::size_t binCount,
                        double dbMin, double dbMax);

// ---- the status bar: the IA's seven cards, in order, from the table's state ----
enum class CardTone { Neutral, Good, Warn, Err };
struct StatusCardModel {
    std::string key;
    std::string value;
    std::string detail;                                    // the one-line detail beside the value
    CardTone tone = CardTone::Neutral;
    bool lampShown = false;
    bool lampLit = false;
    bool lampRed = false;
    std::vector<std::string> lines;                        // the detail lines a click expands
};
// What the status cards need beyond FoxReceiverState: read from the table's lists by the host.
struct HostExtras {
    std::string gainText;                                  // "32 dB", "AUTO", "N/A"
    std::string antenna;                                   // "" when the radio has one antenna
    double ppm = 0.0;
    bool havePpm = false;                                  // the table does not publish the correction yet: shown only when known
    int webPort = 0;
    std::string webBind;
    std::string webRefused;                                // the refusal sentence, "" none
    double outputRate = 0.0;                               // decoder output lines per second
    double outputMeanS = 0.0;
    bool outputMeasured = false;
    std::string recorderValue;                             // "IQ + AUDIO", "FULL", "STALLED"; "" = derive from the flags
    std::string recorderDetail;
};
std::vector<StatusCardModel> statusCards(const FoxReceiverState& state, const HostExtras& extras);

// The decoder output rate over a window of at least two seconds, "measuring" until the window has filled.
class OutputRate {
public:
    void add(std::uint64_t linesTotal, std::int64_t nowMs);
    bool measured() const;
    double perSecond() const;
    double meanSeconds() const;
    void reset();

private:
    struct Sample { std::uint64_t total; std::int64_t ms; };
    std::deque<Sample> samples_;
    static constexpr std::int64_t kWindowMs = 2000;
};

// ---- lines and hints the main screen shows ----
std::string rateLine(const FoxReceiverState& state);                 // "2.400 MS/s · ch 12.5 kHz · underruns 0"
std::string sourceLine(const FoxReceiverState& state, const std::string& band);  // "RTL-SDR Blog V4 · 2 m amateur"
std::string modeHint(const FoxReceiverState& state);                 // "Passband 12.5 kHz · step 12.5 kHz · more in Signal › Radio"
// ---- the bookmark names on the spectrum (docs 6.52) ----
// The row each bookmark's name lands on, or -1 for a name left off (its tick still shows). The classic window's rule
// (gui/bookmark_marker_geometry.hpp, 0.99.54): favourites are placed first so they win whichever row they land on;
// a name's extent runs from 3 px right of its tick for charPx a character, must clear 6 px of every name already on
// a row, and tries the rows from the top; a name that would run off the right edge, or an empty one, is not placed.
// `rows` is the budget: 1 with "Stack close names" off, 3 (kBookmarkNameMaxRows) with it on. One answer per mark,
// in the marks' own order.
struct BookmarkName { double pos = 0.0; std::string label; bool favourite = false; };   // pos 0..1 of the plot width
constexpr int kBookmarkNameMaxRows = 3;
constexpr double kBookmarkNameGapPx = 6.0;
std::vector<int> bookmarkNameRows(const std::vector<BookmarkName>& marks, double widthPx, double charPx, int rows);

std::string tuneHint(bool converterActive);
std::string biasHint(bool present, bool on);
std::string waterfallFootRight(const FoxReceiverState& state, const std::string& decoder);  // "NFM · decoding: APRS", "STOPPED"

}  // namespace foxsdr::slinthost
