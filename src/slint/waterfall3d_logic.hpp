// waterfall3d_logic.hpp - THE 3D WATERFALL SURFACE'S DATA AND WORDS (docs/engine-stage4.md 6.64, specs/THREED.md section 1): the history of spectrum rows the surface is built from, the
// resampling of it to the grid the renderer meshes, and the numbers' formatting for the axes. Pure: no Slint, no engine, no window.
//
// THE DATA IS THE LIVE WATERFALL'S. The 2D waterfall keeps only colours (a picture); the 3D surface needs the levels, so the host keeps ONE row of levels per published frame as well (the
// window's maxima in kWf3dStoreCols columns, a few microseconds a frame) and trims what is older than kWf3dMaxAgeS. Turning the 3D view on therefore shows the history that is already there.
// A change of the frequency window (a zoom, a pan, a retune) starts the history again, as the 2D picture's columns would no longer line up with it: `windowKey` says which window a row is of.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace foxsdr::slintview3d {

constexpr int kWf3dStoreCols = 192;                              // the columns a stored row has
constexpr double kWf3dMaxAgeS = 125.0;                           // the longest HISTORY DEPTH (120 s) and a little more
constexpr std::array<double, 3> kWf3dDepthsS{30.0, 60.0, 120.0}; // HISTORY DEPTH's three choices, seconds
constexpr std::size_t kWf3dMaxRows = 4096;                       // a hard cap whatever the frame rate

class WaterfallHistory {
public:
    // One published frame: `n` levels (dB) of the window `windowKey` at time `tMs` (milliseconds, any steady clock). A new key empties the history first.
    void push(std::int64_t tMs, const float* bins, std::size_t n, std::uint64_t windowKey);
    void clear();
    std::size_t size() const { return rows_.size(); }
    // Seconds between the oldest stored row and `nowMs`; 0 when there is none.
    double spanSeconds(std::int64_t nowMs) const;
    // The surface's grid: `rows` x `cols` levels, row-major, row 0 the NEWEST (age 0) and row r of age r / (rows - 1) * depthS; column c the maximum over its share of the stored columns.
    // A row with no stored row at or before its age is NaN (a hole in the surface: the history is shorter than the depth). Returns how many rows have data.
    std::size_t grid(std::int64_t nowMs, double depthS, int rows, int cols, std::vector<float>& out) const;

private:
    struct Row {
        std::int64_t t = 0;
        std::array<float, kWf3dStoreCols> v{};
    };
    std::deque<Row> rows_;
    std::uint64_t key_ = 0;
    bool haveKey_ = false;
};

// The surface's level of detail from the well's size in pixels: a grid cell is never under about 8 px wide or 6 px deep, between a coarse floor and the stored resolution.
void gridSizeFor(int pixelW, int pixelH, int& rows, int& cols);

// "145.500" - a frequency in MHz with as many decimals as the labels' spacing needs (stepHz is the distance between two neighbouring labels): 3 for a step of 1 kHz or more down to 6 below.
std::string formatMHz(double hz, double stepHz);
// "42" - whole seconds, or one decimal under a 10 s step.
std::string formatAge(double seconds, double stepS);
// "-100": whole dB.
std::string formatDb(double db);

}  // namespace foxsdr::slintview3d
