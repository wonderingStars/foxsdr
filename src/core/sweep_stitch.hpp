// sweep_stitch.hpp - THE WIDE-BAND STITCHER and the time-by-frequency HEATMAP STORE.
//
// WHAT IT IS FOR. An ordinary SDR (RTL-SDR, B200, anything) sees one sample
// rate's worth of spectrum at a time. A wideband picture wider than that is built
// by tuning to a series of centres, taking an FFT at each, and putting the pieces
// side by side. This file is the host-side model of that, with no radio in it:
// it takes the finished captures (centre, rate, FFT bin levels in dB) and gives
// back ONE wide trace per pass, ready for the heatmap and for click-to-tune. It
// is reusable by the main spectrum and, through a hand-off, by any plugin.
//
// THE CAPTURE. db[i], i = 0..N-1, is the level of bin i of an FFT whose bin N/2
// is the centre: bin i sits at  centre + (i - N/2) * rate / N  (the layout of an
// fftshift). Levels in dB, NaN = no data. The caller's capture carries whatever
// gain the receiver had at that moment; the stitcher does not know it.
//
// WHAT THE STITCHER DOES, in order:
//  1. Trim the roll-off. The outer `trimFraction` of the bins at EACH end of every
//     capture is thrown away (default 12 %: where a receiver's anti-alias filter
//     droops and aliases fold in). What is kept is bins [ceil(t*N), N - ceil(t*N)).
//  2. Put the kept bins on one frequency grid whose cell is the bin width (the
//     median of the captures'); a bin goes to its nearest cell.
//  3. ALIGN LEVELS in the overlaps. Captures are taken in ascending order of centre;
//     the lowest is the reference (0 dB correction). For each pair of neighbours the
//     cells both cover give differences (lower - upper). Their median and MAD
//     reject outliers (a burst present in one capture only): differences further
//     than `outlierMads` robust sigmas (1.4826 * MAD, at least 0.1 dB) from the
//     median are dropped and the offset is the mean of the rest. The upper
//     capture is corrected by the sum along the chain. Fewer than `minOverlapBins`
//     usable differences = the seam is NOT aligned (correction carried over
//     unchanged) and flagged in the result. An offset beyond `maxOffsetDb` refuses
//     the pass.
//  4. Each cell takes the capture whose CENTRE is nearest (the more central bin of
//     two overlapping ones: no averaging, no blend), corrected. Cells no capture
//     covers are NaN (a gap).
//
// WHAT IS RECOVERED, with the stated limits (proved in tests/test_sweep_stitch.cpp):
// tone frequencies to one bin plus the receiver's own tuning error (a common ppm
// shift is not visible to any stitcher), levels relative to the reference capture
// to well under 1 dB, seams within 1 dB. A tone that falls BETWEEN two grid
// cells because the captures' bin grids do not line up loses up to the window's
// scalloping (1.4 dB for a Hann window) - inherent to a single FFT, not the
// stitcher.
//
// THE HEATMAP STORE keeps successive wide traces as rows (time down, frequency
// across) in BOUNDED memory: at most `maxCols` x `maxRows` cells whatever is fed.
// Frequency is decimated by max or mean when a trace has more points than
// columns; time is decimated when the rows are full: neighbouring rows merge
// pairwise by max or mean and every later row then holds twice as many passes
// (uniform time resolution, halving each time the store fills). click-to-tune is
// frequencyAtColumn / columnAtFrequency / frequencyAtFraction.
//
// Nothing here is personal data; memory only; no files, no network.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sweep_line.hpp"

namespace cascade::core {

struct SpectrumCapture {
    double centerHz = 0.0;
    double rateHz = 0.0;       // the span the N bins cover
    std::vector<float> db;     // bin levels, dB, NaN = none
    double timeS = 0.0;
};

struct StitchConfig {
    double trimFraction = 0.12;          // each edge; 0 <= t < 0.45
    std::size_t minOverlapBins = 8;
    double outlierMads = 3.0;
    double maxOffsetDb = 40.0;
    double maxBinWidthMismatch = 0.05;   // captures' bin widths must agree this closely
    std::size_t maxCells = std::size_t{1} << 22;
    std::size_t maxCaptures = 4096;
};

struct StitchSeam {
    double lowerCenterHz = 0.0;
    double upperCenterHz = 0.0;
    std::size_t overlapCells = 0;   // cells both captures cover
    std::size_t usedCells = 0;      // after outlier rejection
    double offsetDb = 0.0;          // this seam's step (lower - upper level)
    bool aligned = false;
};

struct StitchResult {
    bool ok = false;
    std::string error;                    // when !ok
    SweepLine line;                       // the wide trace (NaN = gap); timeS = mean capture time
    std::vector<StitchSeam> seams;        // between neighbours in ascending centre order
    std::vector<double> correctionDb;     // per INPUT capture: what was added to its levels
    double binHz = 0.0;                   // the grid cell
};

StitchResult stitchPass(const std::vector<SpectrumCapture>& captures, const StitchConfig& cfg = {});

// --- the heatmap ------------------------------------------------------------

enum class Decimate { Max, Mean };

struct HeatmapConfig {
    double startHz = 0.0;
    double stopHz = 0.0;
    std::size_t cols = 1024;       // frequency columns (1..kHeatmapMaxCols)
    std::size_t maxRows = 512;     // time rows kept (2..kHeatmapMaxRows)
    Decimate freqMode = Decimate::Max;   // points of one trace falling in one column
    Decimate timeMode = Decimate::Max;   // passes merged into one row
};

inline constexpr std::size_t kHeatmapMaxCols = 65536;
inline constexpr std::size_t kHeatmapMaxRows = 65536;
inline constexpr std::size_t kHeatmapMaxCells = std::size_t{16} << 20;

class SweepHeatmap {
public:
    // false and *why set for an unusable configuration.
    bool configure(const HeatmapConfig& cfg, std::string* why = nullptr);

    // Add one trace as the newest pass. Refused (false) when the line is invalid
    // or does not overlap [startHz, stopHz] at all. Points outside the range are
    // ignored, NaN points are ignored.
    bool addLine(const SweepLine& line, std::string* why = nullptr);

    std::size_t rows() const { return rowCount_; }
    std::size_t cols() const { return cfg_.cols; }
    std::size_t passesPerRow() const { return passesPerRow_; }
    std::size_t passesAdded() const { return passes_; }
    // dB, or NaN when the cell holds nothing (or the indices are out of range).
    float at(std::size_t row, std::size_t col) const;
    // Mean time of the passes in the row.
    double rowTimeS(std::size_t row) const;
    // Bytes the store holds for cells and row bookkeeping; never above the bound
    // maxRows * cols * 8 + maxRows * 16.
    std::size_t memoryBytes() const;

    // click-to-tune: bin <-> frequency. The centre of column c; the column a
    // frequency falls in (-1 outside the range); the frequency at a horizontal
    // position 0..1 across the picture (clamped).
    double frequencyAtColumn(std::size_t col) const;
    long long columnAtFrequency(double hz) const;
    double frequencyAtFraction(double x01) const;
    double columnWidthHz() const { return colHz_; }

private:
    void compact();
    HeatmapConfig cfg_;
    bool configured_ = false;
    double colHz_ = 0.0;
    std::size_t rowCount_ = 0;
    std::size_t passesPerRow_ = 1;
    std::size_t passes_ = 0;
    std::vector<float> value_;            // maxRows * cols
    std::vector<std::uint32_t> count_;    // observations per cell
    std::vector<std::uint32_t> rowPasses_;
    std::vector<double> rowTimeSum_;      // sum of pass times in the row
};

}  // namespace cascade::core
