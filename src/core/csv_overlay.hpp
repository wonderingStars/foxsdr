// CUSTOM CSV COLUMNS OVERLAID FROM A STATION DATABASE.
//
// The user has a station list of their own (a CSV with a frequency column and any other columns: station name, language, transmitter site, power,
// schedule). This model reads it ONCE, keeps the frequency and the columns the user chose, and answers "what does the database say about this
// frequency" so a bookmark list, a marker list or the spectrum can show those columns beside what it already has. It changes no bookmark and writes
// nothing: it is a read-only overlay.
//
// OverlayConfig says which column holds the frequency (matched by header name, ASCII case-insensitive), in which unit (Hz, kHz or MHz), which
// columns to keep (at most 16, in that order) and the delimiter (',' ';' or TAB). It is saved as versioned JSON (version 1; another version or any
// invalid value is refused and changes nothing).
//
// load() parses with core/csv_text.hpp (bounded, strict), finds the columns (a missing one refuses the whole load, naming it), and keeps every row
// whose frequency is a plain decimal number in range after the unit is applied (finite, > 0, <= 1e13 Hz). Other rows are skipped and COUNTED
// (skippedRows()): a database with a few bad lines still loads. A cell that is not valid UTF-8 or holds a control character (other than TAB / line
// break, which become a space) is cleaned the same way, never dropped silently: cleanedCells() counts them. Rows are kept sorted by frequency.
//
// lookup(hz, tolHz) returns the row nearest to hz within tolHz (the lower frequency on a tie) as (column name, value) pairs in the configured order,
// or an empty list. lookupAll() returns every row within tolHz, nearest first, at most `max`.
//
// THREADS: load() on one thread; lookups are const and may run from several after the load has finished.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/csv_text.hpp"
#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

enum class FreqUnit { Hz = 0, kHz = 1, MHz = 2 };

struct OverlayConfig {
    std::string freqColumn = "frequency";
    FreqUnit unit = FreqUnit::kHz;
    std::vector<std::string> columns;
    char delimiter = ',';
};

inline bool validOverlayConfig(const OverlayConfig& c) {
    if (c.freqColumn.empty() || !validUserText(c.freqColumn, 64) || c.columns.empty() || c.columns.size() > 16) { return false; }
    if (c.delimiter != ',' && c.delimiter != ';' && c.delimiter != '\t') { return false; }
    for (const std::string& n : c.columns) { if (n.empty() || !validUserText(n, 64)) { return false; } }
    return true;
}

inline nlohmann::json overlayConfigToJson(const OverlayConfig& c) {
    const char* unit = c.unit == FreqUnit::Hz ? "Hz" : c.unit == FreqUnit::kHz ? "kHz" : "MHz";
    return {{"version", 1}, {"freqColumn", c.freqColumn}, {"unit", unit}, {"columns", c.columns}, {"delimiter", std::string(1, c.delimiter)}};
}

inline bool overlayConfigFromJson(const nlohmann::json& j, OverlayConfig& out, std::string* err = nullptr) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (!j.is_object()) { return fail("not an object"); }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail("unknown version"); }
    if (!j.contains("freqColumn") || !j["freqColumn"].is_string() || !j.contains("unit") || !j["unit"].is_string() || !j.contains("columns") ||
        !j["columns"].is_array() || !j.contains("delimiter") || !j["delimiter"].is_string()) {
        return fail("a field is missing or of the wrong type");
    }
    OverlayConfig c;
    c.freqColumn = j["freqColumn"].get<std::string>();
    const std::string u = j["unit"].get<std::string>();
    if (u == "Hz") { c.unit = FreqUnit::Hz; } else if (u == "kHz") { c.unit = FreqUnit::kHz; } else if (u == "MHz") { c.unit = FreqUnit::MHz; } else { return fail("unknown unit"); }
    for (const auto& e : j["columns"]) {
        if (!e.is_string()) { return fail("a column name is not text"); }
        c.columns.push_back(e.get<std::string>());
    }
    const std::string d = j["delimiter"].get<std::string>();
    if (d.size() != 1) { return fail("the delimiter is not one character"); }
    c.delimiter = d[0];
    if (!validOverlayConfig(c)) { return fail("a value is not valid"); }
    out = std::move(c);
    return true;
}

class StationOverlay {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    // All or nothing: on false the overlay is empty and `err` says why.
    bool load(const std::string& csv, const OverlayConfig& cfg, std::string& err) {
        clear();
        if (!validOverlayConfig(cfg)) { err = "the overlay settings are not valid"; return false; }
        std::vector<std::vector<std::string>> rows;
        CsvLimits lim;
        if (!parseCsv(csv, rows, lim, err, cfg.delimiter)) { return false; }
        if (rows.empty()) { err = "the file is empty"; return false; }
        const std::vector<std::string>& head = rows[0];
        const auto find = [&](const std::string& name) -> int {
            for (std::size_t i = 0; i < head.size(); ++i) { if (iequals(head[i], name)) { return static_cast<int>(i); } }
            return -1;
        };
        const int fc = find(cfg.freqColumn);
        if (fc < 0) { err = "no column named " + cfg.freqColumn; return false; }
        std::vector<int> cols;
        for (const std::string& n : cfg.columns) {
            const int c = find(n);
            if (c < 0) { err = "no column named " + n; return false; }
            cols.push_back(c);
        }
        const double scale = cfg.unit == FreqUnit::Hz ? 1.0 : cfg.unit == FreqUnit::kHz ? 1e3 : 1e6;
        names_ = cfg.columns;
        for (std::size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (static_cast<std::size_t>(fc) >= row.size()) { ++skipped_; continue; }
            const std::string& f = row[static_cast<std::size_t>(fc)];
            char* end = nullptr;
            const double v = std::strtod(f.c_str(), &end);
            if (f.empty() || *end != '\0' || !std::isfinite(v)) { ++skipped_; continue; }
            const double hz = v * scale;
            if (!(hz > 0.0) || hz > 1e13) { ++skipped_; continue; }
            Row out;
            out.hz = hz;
            for (const int c : cols) {
                std::string cell = static_cast<std::size_t>(c) < row.size() ? row[static_cast<std::size_t>(c)] : std::string();
                if (!validUserText(cell, 4096)) { cell = clean(cell); ++cleaned_; }
                out.cells.push_back(std::move(cell));
            }
            rows_.push_back(std::move(out));
        }
        std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.hz < b.hz; });
        return true;
    }

    void clear() { rows_.clear(); names_.clear(); skipped_ = cleaned_ = 0; }
    std::size_t size() const { return rows_.size(); }
    std::size_t skippedRows() const { return skipped_; }
    std::size_t cleanedCells() const { return cleaned_; }
    const std::vector<std::string>& columnNames() const { return names_; }

    Fields lookup(double hz, double tolHz) const {
        const std::vector<std::size_t> all = within(hz, tolHz, 1);
        return all.empty() ? Fields() : fields(all[0]);
    }
    std::vector<Fields> lookupAll(double hz, double tolHz, std::size_t max = 8) const {
        std::vector<Fields> out;
        for (const std::size_t i : within(hz, tolHz, max)) { out.push_back(fields(i)); }
        return out;
    }
    double frequencyOf(std::size_t i) const { return i < rows_.size() ? rows_[i].hz : 0.0; }

private:
    struct Row { double hz = 0.0; std::vector<std::string> cells; };

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) { return false; }
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
            if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
            if (x != y) { return false; }
        }
        return true;
    }
    // Control characters (and a broken UTF-8 byte) become a space; the result is always valid user text.
    static std::string clean(const std::string& in) {
        std::string o;
        for (std::size_t i = 0; i < in.size();) {
            std::string one;
            const unsigned char c = static_cast<unsigned char>(in[i]);
            const std::size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
            if (len > 0 && i + len <= in.size()) { one = in.substr(i, len); }
            if (!one.empty() && validUserText(one, 4) && !(len == 1 && c < 0x20)) { o += one; i += len; }
            else { o.push_back(' '); ++i; }
        }
        return validUserText(o, 4096) ? o : std::string();
    }
    Fields fields(std::size_t i) const {
        Fields f;
        for (std::size_t c = 0; c < names_.size(); ++c) { f.emplace_back(names_[c], rows_[i].cells[c]); }
        return f;
    }
    // Indices of rows within tol of hz, nearest first (lower frequency first on a tie).
    std::vector<std::size_t> within(double hz, double tolHz, std::size_t max) const {
        std::vector<std::size_t> idx;
        if (!std::isfinite(hz) || !std::isfinite(tolHz) || tolHz < 0.0 || rows_.empty() || max == 0) { return idx; }
        const auto lo = std::lower_bound(rows_.begin(), rows_.end(), hz - tolHz, [](const Row& r, double v) { return r.hz < v; });
        for (auto it = lo; it != rows_.end() && it->hz <= hz + tolHz; ++it) { idx.push_back(static_cast<std::size_t>(it - rows_.begin())); }
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return std::fabs(rows_[a].hz - hz) < std::fabs(rows_[b].hz - hz); });
        if (idx.size() > max) { idx.resize(max); }
        return idx;
    }

    std::vector<Row> rows_;
    std::vector<std::string> names_;
    std::size_t skipped_ = 0, cleaned_ = 0;
};

}  // namespace cascade::core::tuning
