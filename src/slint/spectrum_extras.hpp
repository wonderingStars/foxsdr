// spectrum_extras.hpp - THE SPECTRUM AND WATERFALL EXTRAS OF SCREEN LANE B (docs/multi-radio-records/screen-b.md): the colour maps, the waterfall's line rate, its clock stamps, the reference
// trace, the automatic peak labels, the A/B measurement markers, the SNR and channel-power readouts, the frequency tracker, the signal history with its CSV, and the painters of the measurement
// box and the history graph. Pure: no Slint, no engine, no clock (every time is passed in); the host (main.cpp) calls them from paneTick and the menus, test_slint_host_extras pins them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "host_logic.hpp"

namespace foxsdr::slintextras {

using foxsdr::slinthost::Rgba;
using foxsdr::slinthost::Tick;

// ---------------------------------------------------------------- S7: the waterfall's colour maps
// 0 = the classic map (host_logic heat()), 1..5 = the OKLab maps of core/colour_maps.hpp (Deep sea, Ember, Slate, Spectrum, Blue-yellow safe).
constexpr int kPaletteCount = 6;
const char* paletteName(int index);                              // the English name; the host translates the label it shows
const std::array<Rgba, 256>& paletteTable(int index);            // index clamped
inline std::size_t paletteIndexOf(float t01) { return static_cast<std::size_t>(t01 <= 0.0f ? 0.0f : t01 >= 1.0f ? 255.0f : t01 * 255.0f + 0.5f); }

// ---------------------------------------------------------------- S10: the waterfall's line rate
// Option 0 is "every frame" (the engine's frame rate); the others are lines a second, the slowest one a line a minute.
constexpr int kSpeedCount = 10;
double speedLinesPerSecond(int index);                           // clamped; 0 = every frame
// A line is made from the STRONGEST level of each bin over the frames since the last line, so a short signal is not lost to the slow rate.
class RowPacer {
public:
    void setLinesPerSecond(double lps);
    double linesPerSecond() const { return lps_; }
    // Feeds one frame (its `n` bins) at time `nowS` (steady seconds). Returns true when a line is due: `row()` is then that line.
    bool feed(const float* bins, std::size_t n, double nowS);
    const std::vector<float>& row() const { return row_; }
    void reset() { acc_.clear(); row_.clear(); lastS_ = -1e18; lines_ = 0; }
    std::uint64_t lines() const { return lines_; }
private:
    double lps_ = 0.0;
    std::vector<float> acc_, row_;
    double lastS_ = -1e18;
    std::uint64_t lines_ = 0;
};

// ---------------------------------------------------------------- S8: clock stamps down the waterfall
// Ticks at round clock times (the step grows with the line spacing: 1 s ... 1 h) at their place down a picture of `heightPx` lines whose newest line (row 0) was made at `newestUtcS`
// and where a line is `secondsPerRow` apart. mode 1 = UTC, 2 = local (`localOffsetS` east of UTC). frac 0 = the newest line, 1 = the oldest. Labels: HH:MM:SS, HH:MM from a minute up.
std::vector<Tick> clockTicks(double newestUtcS, double secondsPerRow, double heightPx, double minPitchPx, int mode, double localOffsetS);
std::string clockText(double utcS, int mode, double localOffsetS, bool seconds);

// ---------------------------------------------------------------- S22: the reference trace
struct ReferenceTrace {
    bool set = false;
    std::vector<float> bins;                                     // the frame saved (dBFS per bin)
    double centreHz = 0, spanHz = 0;
    void save(const float* frame, std::size_t n, double centre, double span);
    void clear() { set = false; bins.clear(); }
    // The saved levels at each of `w` columns of the window (viewCentre, viewSpan); a column the saved frame does not cover is NaN.
    void columns(double viewCentreHz, double viewSpanHz, std::size_t w, std::vector<float>& out) const;
};

// ---------------------------------------------------------------- S20: automatic peak labels
struct Peak { std::size_t bin = 0; float db = 0.0f; double hz = 0.0; };
// The `maxCount` strongest local maxima that stand at least `aboveFloorDb` over the median of the frame and at least `minSepBins` from a stronger one, left to right.
// The frequency is refined by a parabola through the peak and its neighbours.
std::vector<Peak> findPeaks(const float* bins, std::size_t n, double centreHz, double spanHz, int maxCount, double aboveFloorDb, std::size_t minSepBins);

// ---------------------------------------------------------------- S14 / Q2: SNR and channel power
double medianDb(const float* bins, std::size_t n);               // the median level of the bins (a copy is partially sorted)
struct SnrReading { bool valid = false; double peakDb = 0, floorDb = 0, snrDb = 0; };
// The strongest bin of [p0, p1) against the median of the bins OUTSIDE it (of the whole frame when the passband is most of it). The median of noise-only power bins reads about 1.6 dB under
// the mean power (ln 2), so the SNR it gives is about 1.6 dB higher than one against the mean noise power.
SnrReading measureSnr(const float* bins, std::size_t n, std::size_t p0, std::size_t p1);
// 10 log10 of the sum of the bins' power in [b0, b1): the bins' own dBFS, summed (relative: not corrected for the window's noise bandwidth). -200 when empty.
double channelPowerDb(const float* bins, std::size_t n, std::size_t b0, std::size_t b1);
// The level at a frequency: the strongest of the bin it falls in and its two neighbours; NaN outside the frame.
double levelAtHz(const float* bins, std::size_t n, double centreHz, double spanHz, double hz);

// ---------------------------------------------------------------- S21: A / B measurement markers
struct AbMarkers {
    bool aSet = false, bSet = false;
    double aHz = 0, bHz = 0;
    void setA(double hz) { aSet = true; aHz = hz; }
    void setB(double hz) { bSet = true; bHz = hz; }
    void clear() { aSet = bSet = false; }
};
struct AbReading { bool both = false; double deltaHz = 0, deltaDb = 0, aDb = 0, bDb = 0; };
AbReading abReading(const AbMarkers& ab, const float* bins, std::size_t n, double centreHz, double spanHz);
std::string abLabelA(const AbMarkers& ab, double levelDb);
std::string abLabelB(const AbMarkers& ab, double levelDb, const AbReading& r);

// ---------------------------------------------------------------- Q3: the frequency tracker
struct TrackReading { bool valid = false; double hz = 0, offsetHz = 0, driftHzPerS = 0, peakDb = 0; };
class FreqTracker {
public:
    void reset() { t_.clear(); f_.clear(); }
    // The strongest bin inside [lowHz, highHz], refined by a parabola over its neighbours in dB, valid when it stands `minAboveFloorDb` over the frame's median.
    // `nowS` is a steady clock; `refHz` is what the offset is measured from (the tuned frequency). Drift is the slope of a line through the last `windowS` seconds.
    TrackReading update(double nowS, const float* bins, std::size_t n, double centreHz, double spanHz, double lowHz, double highHz, double refHz, double minAboveFloorDb = 8.0, double windowS = 10.0);
private:
    std::vector<double> t_, f_;
};

// ---------------------------------------------------------------- S15: the signal history
struct HistorySample { double utcS = 0; float peakDb = 0, floorDb = 0, snrDb = 0, powerDb = 0; };
class SignalHistory {
public:
    // At most one sample per `minGapS` seconds, the newest `capacity` kept.
    void push(const HistorySample& s, double minGapS = 1.0);
    void clear() { v_.clear(); }
    const std::vector<HistorySample>& samples() const { return v_; }
    std::size_t capacity = 3600;
    // CSV: `#` lines (what it is, the units), a header, then one line a sample: UTC (ISO 8601, to the second), peak_dbfs, floor_dbfs, snr_db, power_dbfs (one decimal).
    std::string csv(const std::string& radioLabel) const;
private:
    std::vector<HistorySample> v_;
};

// ---------------------------------------------------------------- the painters into the trace picture (RGBA, transparent where nothing is drawn)
// The measurement box: lines of text in the 5 x 7 face (capitals, digits, signs: nothing to translate) in the picture's top right corner over a dark plate.
void paintMeasureBox(Rgba* px, std::uint32_t w, std::uint32_t h, const std::vector<std::string>& lines);
// The history graph in the picture's bottom right corner: `which` 0 = SNR (dB), 1 = peak (dBFS), 2 = power (dBFS) over the newest `seconds` seconds; returns the number of samples drawn.
std::size_t paintHistoryGraph(Rgba* px, std::uint32_t w, std::uint32_t h, const SignalHistory& hist, int which, double seconds);

}  // namespace foxsdr::slintextras
