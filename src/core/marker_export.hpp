// The MARKER DATABASE, edited and exported separately from the bookmarks.
//
// core/freq_markers.hpp is the store (drop, remove, note, save to markers.json). What it lacks is the way OUT and IN: a marker list to edit in a
// spreadsheet or hand to someone, and back. This header is that, over the existing FreqMarker record and the FreqMarkers class's own add() / setNote().
//
// CSV: header `number,freq_hz,noted_unix,note`, one marker a line, frequency as hertz with three decimals, the note quoted when it needs it. With
// `spreadsheetSafe` a note that starts with = + - @ gets a leading ' (core/csv_text.hpp); the reader removes one such guard. A note that really begins
// with '= is therefore read back without its apostrophe: the cost of the guard, stated here and in the test.
// JSON: {"version":1,"markers":[{"n":1,"freqHz":..,"noted":..,"note":".."}]}, exact.
//
// IMPORT is strict and all or nothing: the header row must match; every row needs 4 fields, a positive integer number (unique in the file), a finite
// positive frequency up to 1e13 Hz, a non-negative time stamp and a note of valid user text up to 500 bytes; at most FreqMarkers::kMaxMarkers rows.
// importInto() then adds the markers to a live list: a marker within mergeHz of an existing one is merged into it (the existing number is kept, its
// note is replaced only when it has none), the others are added with NEW numbers (numbers are names in a live list and are never reused, so a file's own
// numbers are not kept). It returns how many were added and how many merged, and stops at a full list.
//
// Pure; the caller reads and writes the files. No identifier of a person or device is involved: a frequency, a time and the user's own words.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/csv_text.hpp"
#include "core/freq_markers.hpp"
#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

inline constexpr std::size_t kMarkerNoteMax = 500;

inline bool validMarker(const FreqMarker& m) {
    return m.number > 0 && std::isfinite(m.freqHz) && m.freqHz > 0.0 && m.freqHz <= 1e13 && m.notedUnix >= 0 && validUserText(m.note, kMarkerNoteMax, true);
}

inline std::string markersToCsv(const std::vector<FreqMarker>& list, bool spreadsheetSafe = true) {
    std::string o = "number,freq_hz,noted_unix,note\r\n";
    for (const FreqMarker& m : list) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d,%.3f,%lld,", m.number, m.freqHz, static_cast<long long>(m.notedUnix));
        o += buf;
        o += csvField(m.note, spreadsheetSafe);
        o += "\r\n";
    }
    return o;
}

inline nlohmann::json markersToJson(const std::vector<FreqMarker>& list) {
    nlohmann::json arr = nlohmann::json::array();
    for (const FreqMarker& m : list) { arr.push_back({{"n", m.number}, {"freqHz", m.freqHz}, {"noted", m.notedUnix}, {"note", m.note}}); }
    return {{"version", 1}, {"markers", arr}};
}

inline bool parseMarkersCsv(const std::string& text, std::vector<FreqMarker>& out, std::string& err, bool spreadsheetSafe = true) {
    out.clear();
    std::vector<std::vector<std::string>> rows;
    CsvLimits lim;
    lim.maxBytes = 1u << 20;
    lim.maxRows = FreqMarkers::kMaxMarkers + 1;
    lim.maxCols = 4;
    lim.maxCell = kMarkerNoteMax * 2 + 64;
    if (!parseCsv(text, rows, lim, err)) { return false; }
    if (rows.empty() || rows[0] != std::vector<std::string>{"number", "freq_hz", "noted_unix", "note"}) { err = "the header row is not number,freq_hz,noted_unix,note"; return false; }
    std::set<int> seen;
    std::vector<FreqMarker> list;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const std::string where = "row " + std::to_string(r + 1) + ": ";
        if (row.size() != 4) { err = where + "needs 4 fields"; return false; }
        FreqMarker m;
        char* end = nullptr;
        const long long n = std::strtoll(row[0].c_str(), &end, 10);
        if (row[0].empty() || *end != '\0' || n <= 0 || n > 1000000000LL) { err = where + "the number is not valid"; return false; }
        m.number = static_cast<int>(n);
        m.freqHz = std::strtod(row[1].c_str(), &end);
        if (row[1].empty() || *end != '\0') { err = where + "the frequency is not a number"; return false; }
        const long long t = std::strtoll(row[2].c_str(), &end, 10);
        if (row[2].empty() || *end != '\0') { err = where + "the time is not a number"; return false; }
        m.notedUnix = t;
        m.note = spreadsheetSafe ? csvUnguard(row[3]) : row[3];
        if (!validMarker(m)) { err = where + "a value is out of range"; return false; }
        if (!seen.insert(m.number).second) { err = where + "the number is repeated"; return false; }
        list.push_back(std::move(m));
    }
    if (list.size() > FreqMarkers::kMaxMarkers) { err = "more markers than a list holds"; return false; }
    out = std::move(list);
    return true;
}

inline bool parseMarkersJson(const nlohmann::json& j, std::vector<FreqMarker>& out, std::string& err) {
    out.clear();
    if (!j.is_object()) { err = "not an object"; return false; }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { err = "unknown version"; return false; }
    if (!j.contains("markers") || !j["markers"].is_array()) { err = "markers missing"; return false; }
    if (j["markers"].size() > FreqMarkers::kMaxMarkers) { err = "more markers than a list holds"; return false; }
    std::set<int> seen;
    std::vector<FreqMarker> list;
    for (const auto& e : j["markers"]) {
        if (!e.is_object() || !e.contains("n") || !e["n"].is_number_integer() || !e.contains("freqHz") || !e["freqHz"].is_number() || !e.contains("noted") ||
            !e["noted"].is_number_integer() || !e.contains("note") || !e["note"].is_string()) {
            err = "a marker is damaged";
            return false;
        }
        FreqMarker m;
        const long long n = e["n"].get<long long>();
        if (n <= 0 || n > 1000000000LL) { err = "a number is not valid"; return false; }
        m.number = static_cast<int>(n);
        m.freqHz = e["freqHz"].get<double>();
        m.notedUnix = e["noted"].get<long long>();
        m.note = e["note"].get<std::string>();
        if (!validMarker(m)) { err = "a marker value is out of range"; return false; }
        if (!seen.insert(m.number).second) { err = "a number is repeated"; return false; }
        list.push_back(std::move(m));
    }
    out = std::move(list);
    return true;
}

struct MarkerImportResult {
    int added = 0;
    int merged = 0;
    bool listFull = false;
};

inline MarkerImportResult importInto(FreqMarkers& live, const std::vector<FreqMarker>& in, double mergeHz = 0.0) {
    MarkerImportResult r;
    for (const FreqMarker& m : in) {
        const std::size_t before = live.size();
        const int n = live.add(m.freqHz, m.notedUnix, mergeHz);
        if (n == 0) { r.listFull = live.size() >= FreqMarkers::kMaxMarkers; if (r.listFull) { break; } continue; }
        if (live.size() > before) {
            ++r.added;
            if (!m.note.empty()) { live.setNote(n, m.note); }
        } else {
            ++r.merged;
            bool hasNote = false;
            for (const FreqMarker& e : live.list()) { if (e.number == n) { hasNote = !e.note.empty(); } }
            if (!hasNote && !m.note.empty()) { live.setNote(n, m.note); }
        }
    }
    return r;
}

}  // namespace cascade::core::tuning
