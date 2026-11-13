// wire_b_history.hpp - THE SWEEP HISTORY OF THE SWEEP HOST: the lines kept in memory, and the small file helpers of SWEEP_HISTORY (wire-b second pass, item 37 first half).
//
// WHAT IT KEEPS. Every line the sweep host receives (a tinySA, an RF Explorer, a HackRF in sweep mode, the stitched sweep of an ordinary radio) is also remembered here, newest last, in MEMORY ONLY:
// at most kSweepKeepPoints points in all (the oldest lines go first) and at most kHistoryMaxSweeps lines. Nothing is on disk until a person saves.
//
// THE FILE. SWEEP_HISTORY "save" writes core/sweep_history.hpp's text (the default CSV: a PLAIN TEXT file whose first line is the header `#foxsdr-sweep-history,csv,1`, then the column line
// `time_s,start_hz,stop_hz,points,db`, then one sweep per row; JSON and XML carry the same data) to a full path the person gave; "load" reads such a file back, whichever of the three it is. Rules
// (the marker export's, wire-a): the path is a full path with no control character, under 1024 bytes; a file that exists is never overwritten; nothing larger than kSweepFileBytes is read or
// written; the facts carry words and counts, never a path. The file holds no name, no device identifier, no position (core/sweep_history.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <deque>
#include <string>

#include "core/sweep_history.hpp"
#include "core/sweep_line.hpp"

namespace cascade::engine::wireb {

inline constexpr std::size_t kSweepKeepPoints = 300000;   // about 3.6 MB of CSV
inline constexpr std::size_t kSweepFileBytes = std::size_t{4} << 20;

class SweepKeep {
public:
    void add(const cascade::core::SweepLine& line);   // refuses an empty line; trims the oldest to the budget
    void clear();
    // Replaces the history with `h` (false with `why` when it is over the budget).
    bool replace(const cascade::core::SweepHistory& h, std::string& why);
    cascade::core::SweepHistory snapshot() const;
    std::size_t lines() const { return lines_.size(); }
    std::size_t points() const { return points_; }
    double firstTimeS() const { return lines_.empty() ? 0.0 : lines_.front().timeS; }
    double lastTimeS() const { return lines_.empty() ? 0.0 : lines_.back().timeS; }

private:
    std::deque<cascade::core::SweepLine> lines_;
    std::size_t points_ = 0;
};

// A full path (absolute, a file name, no control character, under 1024 bytes). False with `why` otherwise.
bool historyFullPath(const std::string& path, std::string& why);
// Reads a file up to `maxBytes` (a bigger one, a directory, a missing one: false with `why`; never throws).
bool historyReadFile(const std::string& path, std::size_t maxBytes, std::string& text, std::string& why);
// Writes a NEW file (a temporary file renamed into place; the directory made; one that exists is refused).
bool historyWriteNewFile(const std::string& path, const std::string& text, std::string& why);

}  // namespace cascade::engine::wireb
