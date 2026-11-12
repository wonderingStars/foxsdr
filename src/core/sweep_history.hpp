// sweep_history.hpp - spectrum trace / sweep HISTORY saved and loaded as CSV,
// JSON or XML, and the import of a device's exported frequency/level text.
//
// WHAT IS SAVED: a list of SweepLine (time, start, stop, N dB values). Nothing
// else - no name, no device identifier, no position. Memory in, memory out: the
// functions take and return text; the caller does the file I/O.
//
// THE THREE FORMATS carry the same data and the same version (1). A reader
// refuses any other version.
//
//  CSV   line 1: `#foxsdr-sweep-history,csv,1`
//        line 2: `time_s,start_hz,stop_hz,points,db`      (fixed, checked)
//        line 3..: one sweep per row:
//          time_s,start_hz,stop_hz,points,db_0,db_1,...,db_{points-1}
//        a row has exactly 4 + points fields; an EMPTY db field = no data (NaN);
//        decimal numbers with '.', no thousands separators, no quotes, no "nan"
//        or "inf" text. time/frequency written %.17g, levels %.9g (both read back
//        bit-exact). Line endings LF written, LF or CRLF read. Blank lines are
//        allowed only after the header.
//
//  JSON  {"format":"foxsdr-sweep-history","version":1,"sweeps":[
//          {"time_s":T,"start_hz":F,"stop_hz":F,"db":[L,...]}, ... ]}
//        one sweep per line when written; `null` in db = no data. Unknown keys
//        are refused. Syntax errors report the line of the error; a bad sweep
//        reports the line its object starts on.
//
//  XML   <?xml version="1.0" encoding="UTF-8"?>
//        <sweep-history format="foxsdr-sweep-history" version="1">
//        <sweep time_s="T" start_hz="F" stop_hz="F" points="N">L L L ...</sweep>
//        </sweep-history>
//        levels separated by blanks, `nan` = no data. NO DTD, NO entities, NO
//        CDATA, NO comments, NO other elements or attributes: a hand parser of
//        exactly this schema, so there is no external-entity or expansion
//        surface. Syntax and schema errors report the line.
//
// CAPS (a malformed or hostile file cannot ask for more): kHistoryMaxBytes of
// text, kHistoryMaxSweeps sweeps, kSweepMaxPointsPerLine points in one sweep,
// kHistoryMaxTotalPoints points in all. A writer refuses a history over any of
// them or holding an invalid line, with the reason.
//
// The import of a device's exported text (RF Explorer's "Export Single Signal
// CSV", documented by the maker as one "[frequency (MHz), amplitude (dBm)]" pair
// per line, https://j3.rf-explorer.com/rf-explorer-file-formats) is
// importFreqLevelPairs: one trace out.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/sweep_line.hpp"

namespace cascade::core {

inline constexpr std::size_t kHistoryMaxBytes = std::size_t{64} << 20;        // 64 MiB of text
inline constexpr std::size_t kHistoryMaxSweeps = 100000;
inline constexpr std::size_t kHistoryMaxTotalPoints = std::size_t{16} << 20;  // 16 777 216
inline constexpr int kHistoryVersion = 1;

struct SweepHistory {
    std::vector<SweepLine> sweeps;
    bool operator==(const SweepHistory& o) const { return sweeps == o.sweeps; }
};

struct HistoryLoad {
    bool ok = false;
    SweepHistory history;
    std::size_t line = 0;   // 1-based line of the first problem; 0 = not tied to a line (size caps)
    std::string error;
};

enum class HistoryFormat { Csv, Json, Xml };

// Empty string and *whyNot set when the history cannot be written (invalid line,
// over a cap, empty history).
std::string historyToText(const SweepHistory& h, HistoryFormat f, std::string* whyNot = nullptr);

HistoryLoad historyFromCsv(std::string_view text);
HistoryLoad historyFromJson(std::string_view text);
HistoryLoad historyFromXml(std::string_view text);
// By the first non-blank character: '#' CSV, '{' JSON, '<' XML; else refused at line 1.
HistoryLoad historyFromText(std::string_view text);

struct PairsOptions {
    double freqUnitHz = 1.0e6;          // the frequency column's unit (RF Explorer: MHz)
    double maxSpacingDeviation = 0.01;  // largest allowed |step - mean step| / mean step
};

// One "frequency, level" pair per line, separated by comma, semicolon, tab or
// blanks (any run). Blank lines skipped; a FIRST line that starts with a letter
// or '#' is taken as a header and skipped. Frequencies strictly increasing and
// evenly spaced within the option; at least two pairs; the line numbers of
// refusals are the file's. The resulting history holds ONE sweep (time 0).
HistoryLoad importFreqLevelPairs(std::string_view text, const PairsOptions& opt = {});

}  // namespace cascade::core
