// freq_import_formats - the CHIRP CSV and SDR++ JSON readers (feat/web-bookmarks). See core/freq_import.hpp.
//
// CLEAN-ROOM. Neither program's source was read. The CHIRP header and the SDR++ keys are the ones the survey
// appendix (second-feature-list-survey.md, "SDR++ bookmark JSON" and "CHIRP CSV") records, and the tests'
// fixtures are written by hand to that description. Both formats are therefore marked UNVERIFIED in
// docs/feature-records/web-bookmarks.md until one real export of each has been read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

namespace cascade::core {

namespace {

std::string_view dropBom(std::string_view t) {
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
        static_cast<unsigned char>(t[2]) == 0xBF) {
        t.remove_prefix(3);
    }
    return t;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) { s.remove_prefix(1); }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) { s.remove_suffix(1); }
    return std::string(s);
}

std::string lowerTrim(std::string_view s) {
    std::string o = trimmed(s);
    for (char& c : o) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return o;
}

// A comma-separated record reader that keeps a quoted field whole - commas and line breaks included, "" for a
// quote - and ends a record at LF, CRLF or a lone CR outside quotes. A record that is one empty cell (an empty
// row) is dropped. An unterminated quote runs to the end of the text, which is what a spreadsheet does too.
std::vector<std::vector<std::string>> readRecords(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    bool cellWasQuoted = false;
    const auto endCell = [&]() {
        row.push_back(cellWasQuoted ? cell : trimmed(cell));
        cell.clear();
        cellWasQuoted = false;
    };
    const auto endRow = [&]() {
        endCell();
        const bool empty = row.size() == 1 && row[0].empty();
        if (!empty) { rows.push_back(std::move(row)); }
        row.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.push_back(c);
            }
        } else if (c == '"' && trimmed(cell).empty()) {
            quoted = true;
            cellWasQuoted = true;
            cell.clear();
        } else if (c == ',') {
            endCell();
        } else if (c == '\n') {
            endRow();
        } else if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') { ++i; }
            endRow();
        } else {
            cell.push_back(c);
        }
    }
    if (!cell.empty() || !row.empty() || cellWasQuoted) { endRow(); }
    return rows;
}

bool toNumber(const std::string& s, double& out) {
    const std::string t = trimmed(s);
    if (t.empty()) { return false; }
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

std::string mhzName(double hz) {
    char def[32];
    std::snprintf(def, sizeof(def), "%.4f MHz", hz / 1e6);
    return def;
}

// ---- CHIRP ------------------------------------------------------------------------------------------------

bool isChirpColumn(const std::string& l) {
    static const char* const kNames[] = {"duplex", "offset", "tone", "rtonefreq", "ctonefreq", "dtcscode",
                                         "dtcspolarity", "tstep", "skip", "power", "comment", "crossmode",
                                         "rxdtcscode"};
    for (const char* n : kNames) {
        if (l == n) { return true; }
    }
    return false;
}

struct ChirpHeader {
    bool ok = false;
    int location = -1, name = -1, frequency = -1, tone = -1, rTone = -1, cTone = -1, dtcs = -1, mode = -1;
};

ChirpHeader chirpHeader(const std::vector<std::string>& cells) {
    ChirpHeader h;
    int known = 0;
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
        const std::string l = lowerTrim(cells[static_cast<std::size_t>(i)]);
        if (l == "location" && h.location < 0) { h.location = i; }
        else if (l == "name" && h.name < 0) { h.name = i; }
        else if (l == "frequency" && h.frequency < 0) { h.frequency = i; }
        else if (l == "tone" && h.tone < 0) { h.tone = i; }
        else if (l == "rtonefreq" && h.rTone < 0) { h.rTone = i; }
        else if (l == "ctonefreq" && h.cTone < 0) { h.cTone = i; }
        else if (l == "dtcscode" && h.dtcs < 0) { h.dtcs = i; }
        else if (l == "mode" && h.mode < 0) { h.mode = i; }
        if (isChirpColumn(l)) { ++known; }
    }
    h.ok = h.location >= 0 && h.frequency >= 0 && known >= 2;
    return h;
}

// CHIRP's mode name -> FoxSDR's. `known` is false for a name with no FoxSDR equivalent.
std::string chirpMode(const std::string& chirp, bool& known) {
    const std::string l = lowerTrim(chirp);
    known = true;
    if (l == "fm" || l == "nfm") { return "NFM"; }
    if (l == "wfm") { return "WFM"; }
    if (l == "am" || l == "nam") { return "AM"; }
    if (l == "usb") { return "USB"; }
    if (l == "lsb") { return "LSB"; }
    if (l == "cw" || l == "cwr") { return "CW"; }
    known = false;
    return "NFM";
}

}  // namespace

bool looksLikeChirpCsv(std::string_view text) {
    text = dropBom(text);
    // Only the first row is needed. A header has no quoted line break, so cutting at the first one is safe.
    const std::size_t nl = text.find_first_of("\r\n");
    const auto rows = readRecords(text.substr(0, nl));
    return !rows.empty() && chirpHeader(rows[0]).ok;
}

ImportResult importChirpCsv(std::string_view text) {
    ImportResult r;
    r.format = "CHIRP CSV";
    text = dropBom(text);
    const auto rows = readRecords(text);
    if (rows.empty()) {
        r.error = "the file is empty";
        return r;
    }
    const ChirpHeader h = chirpHeader(rows[0]);
    if (!h.ok) {
        r.error = "not a CHIRP CSV file: the first row must name the Location and Frequency columns and at least two "
                  "more of CHIRP's (Duplex, Offset, Tone, rToneFreq, cToneFreq, DtcsCode, ...)";
        return r;
    }
    const auto cell = [](const std::vector<std::string>& v, int c) -> std::string {
        return (c >= 0 && c < static_cast<int>(v.size())) ? v[static_cast<std::size_t>(c)] : std::string();
    };
    for (std::size_t li = 1; li < rows.size(); ++li) {
        const auto& v = rows[li];
        ++r.entries;
        double mhz = 0.0;
        if (!toNumber(cell(v, h.frequency), mhz) || mhz <= 0.0) {
            ++r.skipped;
            continue;
        }
        // CHIRP writes MHz. A value that can only be hertz (nothing in MHz reaches 100 000) is taken as hertz.
        const double hz = mhz < 100000.0 ? mhz * 1e6 : mhz;
        Bookmark b;
        b.freqHz = std::round(hz);
        const std::string mode = cell(v, h.mode);
        bool known = true;
        b.mode = chirpMode(mode, known);
        if (!known && !trimmed(mode).empty()) { ++r.modeDefaulted; }
        b.bandwidthHz = defaultBandwidthForMode(b.mode);
        b.name = cell(v, h.name);
        if (b.name.empty()) { b.name = mhzName(b.freqHz); }

        // Tone, kept as text: Bookmark has no tone field. "Tone" = a CTCSS tone sent (rToneFreq), "TSQL" = a CTCSS
        // squelch (cToneFreq), "DTCS" = a DCS code, "Cross" = a pair (shown as written). Nothing for an empty or
        // "None" tone column.
        const std::string tone = lowerTrim(cell(v, h.tone));
        std::string note;
        if (tone == "tone" && !cell(v, h.rTone).empty()) { note = "CTCSS " + cell(v, h.rTone) + " Hz"; }
        else if (tone == "tsql" && !cell(v, h.cTone).empty()) { note = "CTCSS " + cell(v, h.cTone) + " Hz"; }
        else if (tone == "dtcs" && !cell(v, h.dtcs).empty()) { note = "DCS " + cell(v, h.dtcs); }
        else if (tone == "cross") {
            note = "CTCSS/DCS cross";
            if (!cell(v, h.rTone).empty()) { note += " " + cell(v, h.rTone) + " Hz"; }
        }
        if (!note.empty()) {
            b.name += " [" + note + "]";
            ++r.toned;
        }
        r.items.push_back(std::move(b));
    }
    if (r.items.empty()) { r.error = "no row had a usable frequency"; }
    return r;
}

// ---- SDR++ ------------------------------------------------------------------------------------------------

namespace {

// UNVERIFIED index map (see the header).
const char* sdrppMode(long long idx, bool& known) {
    static const char* const kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
    known = idx >= 0 && idx <= 7;
    return known ? kModes[idx] : "NFM";
}

bool parseSdrpp(std::string_view text, nlohmann::ordered_json& out) {
    text = dropBom(text);
    out = nlohmann::ordered_json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object() && out.contains("lists") && out["lists"].is_object();
}

}  // namespace

bool looksLikeSdrppJson(std::string_view text) {
    text = dropBom(text);
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) { ++i; }
    if (i >= text.size() || text[i] != '{') { return false; }
    nlohmann::ordered_json j;
    return parseSdrpp(text, j);
}

ImportResult importSdrppJson(std::string_view text) {
    ImportResult r;
    r.format = "SDR++ JSON";
    nlohmann::ordered_json j;
    if (trimmed(dropBom(text)).empty()) {
        r.error = "the file is empty";
        return r;
    }
    if (!parseSdrpp(text, j)) {
        r.error = "not an SDR++ frequency-manager file: it must be JSON with a \"lists\" object";
        return r;
    }
    for (const auto& list : j["lists"].items()) {
        if (!list.value().is_object() || !list.value().contains("bookmarks") || !list.value()["bookmarks"].is_object()) {
            continue;
        }
        for (const auto& bm : list.value()["bookmarks"].items()) {
            ++r.entries;
            const auto& o = bm.value();
            double hz = 0.0;
            if (!o.is_object() || !o.contains("frequency") || !o["frequency"].is_number()) {
                ++r.skipped;
                continue;
            }
            hz = o["frequency"].get<double>();
            if (!std::isfinite(hz) || hz <= 0.0) {
                ++r.skipped;
                continue;
            }
            Bookmark b;
            b.freqHz = std::round(hz);
            b.name = bm.key().empty() ? mhzName(b.freqHz) : bm.key();
            b.group = list.key();
            bool known = false;
            if (o.contains("mode") && o["mode"].is_number_integer()) {
                b.mode = sdrppMode(o["mode"].get<long long>(), known);
            } else {
                b.mode = "NFM";
            }
            if (!known) { ++r.modeDefaulted; }
            double bw = 0.0;
            if (o.contains("bandwidth") && o["bandwidth"].is_number()) { bw = o["bandwidth"].get<double>(); }
            b.bandwidthHz = (std::isfinite(bw) && bw > 0.0) ? bw : defaultBandwidthForMode(b.mode);
            r.items.push_back(std::move(b));
        }
    }
    if (r.items.empty()) { r.error = "the file has no bookmark with a usable frequency"; }
    return r;
}

}  // namespace cascade::core
