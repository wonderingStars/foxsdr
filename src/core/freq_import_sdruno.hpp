// freq_import_sdruno - bookmark import from an SDRuno memory bank (.s1b), chosen by content.
//
// STANDALONE, header-only, beside core/freq_import_foreign.hpp, whose ForeignImportResult and helpers it reuses; core/freq_import.cpp calls it
// from importFrequencyList (one dispatch line). Bookmark is core/bookmark.hpp's.
//
// WHAT AN .s1b IS, from what was opened (docs/feature-records/oss-net.md has the commits and licences):
//   * LinuxSheeple-E/SDRunoPlugin_Fran (README licence text: the 3-clause BSD wording, "Portions are Copyright (C) 2020 Eric A. Cottrell"; no
//     LICENSE file, GitHub shows no licence; its CSV reader is ben-strasser's fast-cpp-csv-parser, BSD-3 text in the same README), commit
//     bdee270f9c8887493f7e7db47d9157fe309005f4, function loadS1bCsvFile in SDRunoPlugin_Fran.cpp: an .s1b is a plain CSV with 8 or 9
//     columns, named by the plugin Frequency, Scan, M, Station, UTC, SubM, Filter, Port and (9-column files, later versions) Threshold;
//     quote handling is "double quote escape" (a doubled quote inside a quoted field is one quote); the column count of the file decides
//     between the two layouts; the station text is cut to 63 characters by the plugin because SDRuno's annotate text has that limit.
//     Only the column list and the structure were taken from it: no code, and the plugin's behaviour is not copied.
//   * Sample banks in https://github.com/CaptDaags/SDRUno_S1B (no licence file; opened ONLY to learn the vocabulary, none of their text is
//     used or shipped, the fixtures in tests/test_freq_import_sdruno.cpp are written here): frequency in HERTZ in column 1 as a plain integer;
//     Scan is Y or empty; the mode column holds AM, FM, USB, LSB or CW (5 codes in 28 banks; the codes of SDRuno's other demodulators, such
//     as SAM, DSB or ECSS, were NOT seen); the sub-mode column holds NFM on narrow-FM rows; Filter is a bandwidth in hertz ("2800", "150")
//     or in kilohertz with a K ("15K", "12K"); Port is the antenna (AntA, AntB); the files may start with a UTF-8 byte-order mark; lines
//     with only commas are blank, and a line with an empty first column and a text in another column ("   ,,,Section title,,,,") is a
//     TITLE LINE of the bank. Whether SDRuno itself writes title lines is UNVERIFIED (it is in the banks people share; the plugin's reader
//     would trip over them); this importer reads them as group names and does not need them.
//
// MAPPING. Frequency (Hz, plain integer; a decimal or a number outside (0, 1e12] makes the row skipped), Station -> name (a missing name becomes
// "<MHz> MHz"), Scan Y -> the tick (Bookmark::scan), the nearest title line above -> the group, Filter -> bandwidth (the mode's default when
// missing or outside 1 Hz .. 20 MHz). Mode: AM -> AM, USB -> USB, LSB -> LSB, CW -> CW, FM -> NFM (WFM when Filter is 100 kHz or more or the
// sub-mode column says WFM); anything else, including an empty cell, is IMPORTED AS NFM and counted in modeDefaulted, and a non-empty raw code
// is kept in the name as " [SDRuno mode CODE]" because a Bookmark has no field for it. Not kept: UTC, Port, Threshold (Bookmark has no field).
//
// HOSTILE INPUT. Refused: more than 16 MiB, a NUL byte, more than 200000 data rows, a line over 4096 bytes (skipped and counted). Names are cut
// to 128 bytes on a UTF-8 boundary, control characters removed, invalid UTF-8 replaced by '?'. A quote that is not closed on its line is a plain
// character (a quoted field never spans lines here).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/freq_import_foreign.hpp"

namespace cascade::core::sdruno {

constexpr const char* kFormatName = "SDRuno memory bank (.s1b)";
constexpr std::size_t kMaxGroupBytes = 64;

// One line split at commas outside quotes; "" inside quotes is one quote. A quote that is not closed on the line is literal.
inline std::vector<std::string> splitCsvLine(std::string_view line) {
    std::vector<std::string> f;
    std::string cur;
    bool inQ = false;
    // find out first whether the line's quotes balance; if not, none of them is special
    std::size_t quotes = 0;
    for (char c : line) { if (c == '"') { ++quotes; } }
    const bool honour = quotes % 2 == 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (honour && c == '"') {
            if (inQ && i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; }
            else { inQ = !inQ; }
        } else if (c == ',' && !inQ) {
            f.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    f.push_back(std::move(cur));
    for (std::string& s : f) { s = foreign::trimmed(s); }
    return f;
}

// A plain integer number of hertz, 1 .. 1e12.
inline bool parseIntegerHz(const std::string& s, double* out) {
    if (s.empty() || s.size() > 13) { return false; }
    for (char c : s) { if (c < '0' || c > '9') { return false; } }
    const double v = std::strtod(s.c_str(), nullptr);
    if (!(v > 0.0) || v > 1.0e12) { return false; }
    *out = v;
    return true;
}

inline bool isScanFlag(const std::string& s) {
    return s.empty() || s == "Y" || s == "y" || s == "N" || s == "n";
}

// True when the first 200 lines hold a data row of an .s1b (8 or more fields, an integer number of hertz first, a Scan cell of Y, N or empty,
// and no ';' separator) or the column header the plugin names. A generic CSV of fewer columns, a gqrx file (';') and XML are not taken.
inline bool looksLikeS1b(std::string_view text) {
    text = foreign::dropBom(text);
    std::size_t pos = 0;
    int lines = 0;
    while (pos < text.size() && lines < 200) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) { e = text.size(); }
        std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        ++lines;
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (line.empty() || line.size() > foreign::kMaxLine) { continue; }
        if (line.find(';') != std::string_view::npos || line.find('<') != std::string_view::npos) { return false; }
        const std::vector<std::string> f = splitCsvLine(line);
        if (f.size() < 8) { continue; }
        double hz = 0;
        if (parseIntegerHz(f[0], &hz) && isScanFlag(f[1])) { return true; }
        if (foreign::lowerTrim(f[0]) == "frequency" && foreign::lowerTrim(f[1]) == "scan" && foreign::lowerTrim(f[2]) == "m") { return true; }
    }
    return false;
}

inline std::string cutUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) { return s; }
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) { --n; }
    return foreign::trimmed(s.substr(0, n));
}

// The mode a code names. `known` is false for a code this importer has no mapping for (it comes back as "NFM").
inline std::string modeFor(const std::string& rawMode, const std::string& subMode, double filterHz, bool* known) {
    const std::string m = foreign::lowerTrim(rawMode);
    *known = true;
    if (m == "am") { return "AM"; }
    if (m == "usb") { return "USB"; }
    if (m == "lsb") { return "LSB"; }
    if (m == "cw") { return "CW"; }
    if (m == "fm") {
        const std::string sm = foreign::lowerTrim(subMode);
        if (sm == "wfm" || filterHz >= 100000.0) { return "WFM"; }
        return "NFM";
    }
    *known = false;
    return "NFM";
}

// Filter cell: hertz, or kilohertz with a K/k suffix. 0 when absent or not a number.
inline double parseFilterHz(const std::string& s) {
    std::string t = foreign::trimmed(s);
    double mult = 1.0;
    if (!t.empty() && (t.back() == 'K' || t.back() == 'k')) { mult = 1000.0; t.pop_back(); }
    if (t.empty() || t.size() > 12 || t.find_first_not_of("0123456789.") != std::string::npos) { return 0.0; }
    const double v = std::strtod(t.c_str(), nullptr) * mult;
    return (v >= 1.0 && v <= 20.0e6) ? v : 0.0;
}

inline ForeignImportResult importS1b(std::string_view text) {
    ForeignImportResult r;
    r.format = kFormatName;
    if (text.size() > foreign::kMaxBytes) { r.error = "the file is larger than 16 MiB"; return r; }
    if (foreign::hasNul(text)) { r.error = "the file holds a NUL byte: it is not a text file"; return r; }
    text = foreign::dropBom(text);
    std::string group;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) { e = text.size(); }
        std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (line.empty()) { continue; }
        if (line.size() > foreign::kMaxLine) { ++r.entries; ++r.skipped; continue; }
        const std::vector<std::string> f = splitCsvLine(line);
        std::string firstText;
        for (const std::string& c : f) { if (!c.empty()) { firstText = c; break; } }
        if (firstText.empty()) { continue; }                                   // a line of commas
        if (f[0].empty()) {                                                    // a title line
            group = cutUtf8(foreign::cleanName(firstText), kMaxGroupBytes);
            continue;
        }
        if (foreign::lowerTrim(f[0]) == "frequency" && f.size() > 2 && foreign::lowerTrim(f[2]) == "m") { continue; }   // the plugin's column header
        if (r.entries >= foreign::kMaxEntries) { r.error = "more than 200000 rows"; break; }
        ++r.entries;
        double hz = 0;
        if (!parseIntegerHz(f[0], &hz)) { ++r.skipped; continue; }
        auto col = [&](std::size_t i) { return i < f.size() ? f[i] : std::string(); };
        Bookmark b;
        b.freqHz = hz;
        b.group = group;
        b.scan = col(1) == "Y" || col(1) == "y";
        const double filter = parseFilterHz(col(6));
        bool known = true;
        b.mode = modeFor(col(2), col(5), filter, &known);
        std::string name = foreign::cleanName(col(3));
        if (!known || foreign::trimmed(col(2)).empty()) {
            ++r.modeDefaulted;
            if (!known && !foreign::trimmed(col(2)).empty()) {
                const std::string raw = foreign::cleanName(col(2).substr(0, 12));
                const std::string suffix = " [SDRuno mode " + raw + "]";
                name = cutUtf8(name, foreign::kMaxName - suffix.size()) + suffix;   // the raw code survives a long name
            }
        }
        if (name.empty()) { name = foreign::defaultName(hz); }
        b.name = name;
        b.bandwidthHz = filter > 0.0 ? filter : foreign::defaultBandwidth(b.mode);
        r.items.push_back(std::move(b));
    }
    if (r.items.empty() && r.error.empty()) { r.error = "no row had a usable frequency"; }
    return r;
}

}  // namespace cascade::core::sdruno
