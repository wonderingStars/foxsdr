// survey3d_logic.hpp - THE SURVEY ENGINE'S 3D SURFACE: DATA AND WORDS (docs/engine-stage4.md 6.64, specs/THREED2.md section 4): the channels the Survey Engine plugin publishes in its panel (frequency,
// width, guess, peak, over floor, seen, band), read back from the panel's cells, ranked, and laid out as a surface over frequency and occupancy. Pure: no Slint, no engine, no window.
//
// WHAT THE HOST HAS AND WHAT IT DOES NOT. The plugin publishes ONE ROW PER CHANNEL, sorted by activity, with the channel's peak, its level over the noise floor and the share of dwells it was seen
// in: no history by hour of day. The design's surface is frequency by HOUR by level; the host cannot draw an hour axis it was never given, so the surface here is frequency by OCCUPANCY (how much
// of the survey the channel was seen in) by level: each channel is a ridge standing at its frequency, as wide as its width, as high as its peak over the floor, and as long as its Seen share. The
// design's TIME WINDOW, PERCENTILE FLOOR, MERGE WIDTH and SWEEP BAND readouts and its EXPORT CSV key need the plugin's configuration and history (docs 6.64 lists them as deferred).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace foxsdr::slintview3d {

// One channel of the panel, as numbers and as the cells said it.
struct SurveyChannel {
    double freqHz = 0.0;
    double widthHz = 0.0;              // 0 when the cell did not say
    double peakDb = 0.0;
    double overDb = 0.0;               // the peak's level over the floor
    double seenPct = 0.0;              // 0..100
    bool haveWidth = false, havePeak = false, haveOver = false, haveSeen = false;
    std::string freqText, seenText, band, guess;   // the cells, verbatim: the rank list quotes them
    std::string raw;                   // every cell of the row, joined by " | " (what a drive script and a bug report read back)
};

// What the panel hands the 3D view each tick.
struct SurveyFeed {
    bool shown = false;                // a Survey Engine panel page exists
    std::vector<SurveyChannel> channels;   // in the panel's order (by activity), only rows that are channels (the status rows are not)
    std::string status;                // the panel's first status row's first cells joined, "" when none
};

// The number at the start of `text` (a sign, digits, one decimal point) and what follows it, lower-cased and trimmed. False when there is no number.
bool parseLeadingNumber(const std::string& text, double& value, std::string& rest);
// A frequency cell: "145.500 MHz", "145500000", "144.8500", "7.074 MHz", "1090 MHz", "0.5 GHz". A bare number under 30 000 is MHz (what a panel would print), above it Hz.
bool parseFrequencyHz(const std::string& text, double& hz);
// A width cell: "12.5 kHz", "3.1 kHz", "9 Hz", "0.2 MHz"; a bare number is Hz.
bool parseWidthHz(const std::string& text, double& hz);
// "-48", "-48 dB", "+31 dB", "31". The dB figure.
bool parseDb(const std::string& text, double& db);
// "86 % of dwells", "41 %", "9", "2/4" (dwells seen of dwells made): the percentage (0..100, clamped).
bool parsePercent(const std::string& text, double& pct);

// A panel row's cells (heading order as the plugin publishes them: Frequency, Width, Guess, Peak dB, Over floor, Seen, Band - found by the HEADINGS, so a reordered panel still reads) to a channel.
// False for a row whose frequency does not parse (a status row).
bool channelOfCells(const std::vector<std::string>& headings, const std::vector<std::string>& cells, SurveyChannel& out);

// The noise floor the ridges stand on: the median of (peak - over) over the channels that have both, else (the lowest peak - 10 dB), else -110.
double floorDb(const std::vector<SurveyChannel>& ch);
// The frequency span the surface covers: the channels' lowest and highest, padded by 6 % of the spread (at least 50 kHz each side); a single channel is given 400 kHz.
void spanOf(const std::vector<SurveyChannel>& ch, double& loHz, double& hiHz);
// The ridge grid: rows x cols levels (dB), row r at occupancy r / (rows - 1) * 100 %, column c at frequency lo + c / (cols - 1) * (hi - lo). A channel stands from occupancy 0 up to its Seen share
// with a raised-cosine cross-section as wide as its width (at least two columns), as high as its peak; the floor elsewhere.
void ridgeGrid(const std::vector<SurveyChannel>& ch, double loHz, double hiHz, double floorDbV, int rows, int cols, std::vector<float>& out);
// The level range of the vertical axis: the floor to the strongest peak + 3 dB, rounded out to 10 dB, never narrower than 30 dB.
void levelRange(const std::vector<SurveyChannel>& ch, double floorDbV, double& loDb, double& hiDb);
// "145.500" style, as many decimals as the span needs.
std::string formatSurveyMHz(double hz, double spanHz);

}  // namespace foxsdr::slintview3d
