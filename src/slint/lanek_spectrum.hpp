// lanek_spectrum.hpp - SCREEN LANE K's SPECTRUM TOOLS (docs/multi-radio-records/screen-k.md): the pure helpers behind the Display body's averaging choice, the gradient editor, the
// limit mask, the detector choice with its RBW readout and the eight-cursor tool. No Slint, no engine call: functions of a frame of dB per bin and a picture of Rgba pixels.
//
//   detectorColumns    one dB value per display column from a frame, by the detector the rfmon lane defines (dsp/rbw_detector.hpp: Peak, NegPeak, Rms, AvgVolt, AvgLog, Sample). The RbwTrace
//                      class of that header works in W/Hz and adds the RBW's own bandwidth to the level; the screen's axis is dBFS per bin, so the reduction is applied to the displayed
//                      bins directly (the definitions are the header's, the tests pin the closed forms it states for white noise).
//   maskLimit/paintMask  the limit mask (core/spectrum_mask.hpp) as lines over the trace
//   paintCursors       the eight cursors (dsp/sigmeas_cursors.hpp) as thin lines with a numbered tab
//   cursorLevel        the strongest displayed bin within a search width of a frequency
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/spectrum_mask.hpp"
#include "dsp/rbw_detector.hpp"
#include "host_logic.hpp"

namespace foxsdr::slintlanek {

// ---- detectors -----------------------------------------------------------------------------------------------------------------------------
// `points` columns over `n` bins (column c covers bins [c*n/points, (c+1)*n/points), at least one bin). With more columns than bins a column repeats its bin. Non-finite bins read as floorDb.
void detectorColumns(const float* dbBins, std::size_t n, std::size_t points, cascade::dsp::RbwDetector det, float* out, float floorDb = -200.0f);

// The RBW the engine's spectrum has: the ENBW of the window it runs (Blackman-Harris: dsp/spectrum.hpp says so) over `bins` bins across `sampleRateHz`. 0 when unknown.
double nativeRbwHz(double sampleRateHz, std::size_t bins);
std::string rbwText(double hz);   // "2.07 kHz", "341 Hz", "1.2 MHz"; "n/a" for 0

// ---- the limit mask ------------------------------------------------------------------------------------------------------------------------
// The limit at a frequency: the lowest upper line / the highest lower line covering it. False when none covers it.
bool maskLimitAt(const cascade::core::Mask& m, double hz, bool upper, double& db);

// Draws the limit lines over a trace picture of w x h pixels showing [loHz, hiHz] and [dbMin, dbMax]: the upper limit in red, the lower in cyan, 2 px thick; a column the line does not cover is left alone.
void paintMask(foxsdr::slinthost::Rgba* px, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, double dbMin, double dbMax, const cascade::core::Mask& m);

// A mask file from text, as the model parses it; `error` is the model's own sentence.
bool parseMaskText(const std::string& text, cascade::core::Mask& out, std::string& error);

// ---- the cursors ---------------------------------------------------------------------------------------------------------------------------
// Draws cursor i (0..7) at its frequency: a 1 px line in a colour of its own and a 7 x 7 tab at the top. Cursors outside the picture's span are not drawn.
void paintCursors(foxsdr::slinthost::Rgba* px, std::uint32_t w, std::uint32_t h, double loHz, double hiHz, const std::array<double, 8>& freqHz, const std::array<bool, 8>& shown);

// The strongest bin of a frame within +-searchHz of freqHz; NaN when the frequency is outside the frame.
double cursorLevel(const float* dbBins, std::size_t n, double centreHz, double spanHz, double freqHz, double searchHz);

}  // namespace foxsdr::slintlanek
