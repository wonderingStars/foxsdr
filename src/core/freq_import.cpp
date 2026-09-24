// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cascade::core {

namespace {

// The five XML entities and numeric character references; anything else is
// left as written rather than guessed at.
std::string unescapeXml(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        bool ok = true;
        if (ent == "amp") { cp = '&'; }
        else if (ent == "lt") { cp = '<'; }
        else if (ent == "gt") { cp = '>'; }
        else if (ent == "quot") { cp = '"'; }
        else if (ent == "apos") { cp = '\''; }
        else if (ent.size() > 1 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string num(ent.substr(hex ? 2 : 1));
            char* end = nullptr;
            cp = std::strtoul(num.c_str(), &end, hex ? 16 : 10);
            ok = end != nullptr && *end == '\0' && !num.empty() && cp > 0 && cp <= 0x10FFFF;
        } else {
            ok = false;
        }
        if (!ok) {
            out.push_back('&');
            continue;
        }
        // UTF-8 encode.
        if (cp < 0x80) { out.push_back(static_cast<char>(cp)); }
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        i = semi;
    }
    return out;
}

std::string escapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) { s.remove_prefix(1); }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) { s.remove_suffix(1); }
    return s;
}

// The value of <tag>...</tag> inside `seg`, or found=false. <tag/> and
// <tag /> are an empty value.
std::string_view field(std::string_view seg, std::string_view tag, bool& found) {
    found = false;
    const std::string open = "<" + std::string(tag);
    std::size_t at = 0;
    while ((at = seg.find(open, at)) != std::string_view::npos) {
        const std::size_t after = at + open.size();
        if (after >= seg.size()) { return {}; }
        const char c = seg[after];
        if (c == '>' ) {
            const std::string close = "</" + std::string(tag) + ">";
            const std::size_t end = seg.find(close, after + 1);
            if (end == std::string_view::npos) { return {}; }
            found = true;
            return seg.substr(after + 1, end - after - 1);
        }
        if (c == '/' || c == ' ') {
            const std::size_t gt = seg.find('>', after);
            if (gt != std::string_view::npos && gt > 0 && seg[gt - 1] == '/') {
                found = true;
                return {};
            }
        }
        at = after;  // a longer tag with this prefix, e.g. <NameX>
    }
    return {};
}

bool parseNumber(std::string_view s, double& out) {
    const std::string t(trim(s));
    if (t.empty()) { return false; }
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return o;
}

// SDR#'s detector names are the same eight FoxSDR has; anything else is kept
// verbatim (FreqManager keeps unknown modes so a newer file survives) - except
// the common spellings a spreadsheet might carry.
std::string normaliseMode(std::string_view m) {
    std::string u = upper(trim(m));
    if (u == "FM") { return "NFM"; }
    if (u == "WBFM" || u == "WIDEFM" || u == "BFM") { return "WFM"; }
    if (u.empty()) { return "NFM"; }
    return u;
}

}  // namespace

double defaultBandwidthForMode(const std::string& mode) {
    if (mode == "WFM") { return 150000.0; }
    if (mode == "AM") { return 10000.0; }
    if (mode == "DSB") { return 6000.0; }
    if (mode == "USB" || mode == "LSB") { return 2800.0; }
    if (mode == "CW") { return 500.0; }
    if (mode == "RAW") { return 150000.0; }
    return 12500.0;  // NFM and anything unknown
}

ImportResult importSdrSharpXml(std::string_view text) {
    ImportResult r;
    r.format = "SDR# XML";
    const std::string_view kOpen = "<MemoryEntry";
    std::size_t at = text.find(kOpen);
    while (at != std::string_view::npos) {
        // "<MemoryEntry>" and not "<MemoryEntryX": the character after the
        // name must end the tag.
        const std::size_t after = at + kOpen.size();
        if (after < text.size() && text[after] != '>' && text[after] != ' ' && text[after] != '\t' &&
            text[after] != '\r' && text[after] != '\n') {
            at = text.find(kOpen, after);
            continue;
        }
        const std::size_t next = text.find(kOpen, after);
        const std::string_view seg =
            text.substr(after, (next == std::string_view::npos ? text.size() : next) - after);
        at = next;
        ++r.entries;

        bool has = false;
        double hz = 0.0;
        const std::string_view f = field(seg, "Frequency", has);
        if (!has || !parseNumber(f, hz) || hz <= 0.0) {
            ++r.skipped;
            continue;
        }
        Bookmark b;
        b.freqHz = hz;
        b.name = unescapeXml(trim(field(seg, "Name", has)));
        b.group = unescapeXml(trim(field(seg, "GroupName", has)));
        const std::string_view det = field(seg, "DetectorType", has);
        b.mode = normaliseMode(has ? det : std::string_view("NFM"));
        double bw = 0.0;
        const std::string_view fb = field(seg, "FilterBandwidth", has);
        b.bandwidthHz = (has && parseNumber(fb, bw) && bw > 0.0) ? bw : defaultBandwidthForMode(b.mode);
        b.favourite = upper(trim(field(seg, "IsFavourite", has))) == "TRUE";
        double shift = 0.0;
        const std::string_view sh = field(seg, "Shift", has);
        if (has && parseNumber(sh, shift) && shift != 0.0) { ++r.shifted; }
        if (b.name.empty()) {
            char def[32];
            std::snprintf(def, sizeof(def), "%.4f MHz", hz / 1e6);
            b.name = def;
        }
        r.items.push_back(std::move(b));
    }
    if (r.entries == 0) { r.error = "no <MemoryEntry> found - is this an SDR# frequencies.xml?"; }
    return r;
}

namespace {

std::vector<std::string> splitCsv(std::string_view line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == sep) {
            out.push_back(std::string(trim(cur)));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::string(trim(cur)));
    return out;
}

}  // namespace

ImportResult importCsv(std::string_view text) {
    ImportResult r;
    r.format = "CSV";
    // Lines, dropping a UTF-8 BOM (Excel writes one).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) { nl = text.size(); }
        std::string_view l = text.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') { l.remove_suffix(1); }
        if (!trim(l).empty()) { lines.push_back(l); }
        at = nl + 1;
    }
    if (lines.empty()) {
        r.error = "the file is empty";
        return r;
    }
    // The separator a line uses most. Picked from the line the columns are
    // actually read from - a title row with no separators in it would
    // otherwise pick ',' for a ';' file.
    const auto pickSep = [](std::string_view line) {
        const char seps[3] = {',', ';', '\t'};
        char s = ',';
        std::ptrdiff_t best = -1;
        for (const char c : seps) {
            const std::ptrdiff_t n = std::count(line.begin(), line.end(), c);
            if (n > best) {
                best = n;
                s = c;
            }
        }
        return s;
    };
    // A unit word in a header cell: MHz, kHz or Hz, else 0 (not stated).
    const auto unitIn = [](const std::string& u) {
        if (u.find("MHZ") != std::string::npos) { return 1e6; }
        if (u.find("KHZ") != std::string::npos) { return 1e3; }
        if (u.find("HZ") != std::string::npos) { return 1.0; }
        return 0.0;
    };

    // WHERE THE LIST STARTS. The first line whose first cell is a number is
    // data in the default column order; the first line with a cell naming a
    // frequency is a header naming the columns. A line that is neither - a
    // "# exported ..." comment, an Excel title row - is not a header just
    // because it is not a number: it is skipped, counted, and the search goes
    // on. Deciding from line 1 alone used to refuse a whole 30 000-entry list
    // with "no frequency column in the header" over one comment line.
    int cFreq = 0, cName = 1, cGroup = 2, cMode = 3, cBw = 4, cFav = -1;
    double unit = 0.0;    // 0 = decide per value
    double bwUnit = 1.0;  // bandwidth is Hz (SDR#'s convention) unless the header says otherwise
    char sep = ',';
    std::size_t startAt = lines.size();
    bool found = false;
    for (std::size_t li = 0; li < lines.size() && !found; ++li) {
        sep = pickSep(lines[li]);
        const std::vector<std::string> h = splitCsv(lines[li], sep);
        double probe = 0.0;
        // The same decimal-comma reading the data rows get, or a first row of
        // "446,00625;PMR 1" is mistaken for a header and lost.
        std::string first0 = h.empty() ? std::string() : h[0];
        if (sep != ',') { std::replace(first0.begin(), first0.end(), ',', '.'); }
        if (!h.empty() && parseNumber(first0, probe)) {
            startAt = li;
            found = true;
            break;
        }
        int hFreq = -1, hName = -1, hGroup = -1, hMode = -1, hBw = -1, hFav = -1;
        double hUnit = 0.0, hBwUnit = 1.0;
        for (int i = 0; i < static_cast<int>(h.size()); ++i) {
            const std::string u = upper(h[static_cast<std::size_t>(i)]);
            if (hFreq < 0 && u.find("FREQ") != std::string::npos) {
                hFreq = i;
                hUnit = unitIn(u);
            } else if (hGroup < 0 && (u.find("GROUP") != std::string::npos || u == "CATEGORY")) {
                hGroup = i;
            } else if (hName < 0 && (u.find("NAME") != std::string::npos || u == "DESCRIPTION" ||
                                     u == "LABEL")) {
                hName = i;
            } else if (hMode < 0 && (u.find("MODE") != std::string::npos ||
                                     u.find("DETECTOR") != std::string::npos ||
                                     u.find("MODULATION") != std::string::npos)) {
                hMode = i;
            } else if (hBw < 0 && (u.find("BANDWIDTH") != std::string::npos ||
                                   u.find("FILTER") != std::string::npos || u == "BW" ||
                                   u.rfind("BW ", 0) == 0 || u.rfind("BW(", 0) == 0)) {
                hBw = i;
                // The frequency column's unit rule, applied to the bandwidth:
                // "Bandwidth (kHz)" + 12.5 is 12.5 kHz, not 12.5 Hz.
                const double bu = unitIn(u);
                if (bu > 0.0) { hBwUnit = bu; }
            } else if (hFav < 0 && u.find("FAV") != std::string::npos) {
                hFav = i;
            }
        }
        if (hFreq >= 0) {
            cFreq = hFreq;
            cName = hName;
            cGroup = hGroup;
            cMode = hMode;
            cBw = hBw;
            cFav = hFav;
            unit = hUnit;
            bwUnit = hBwUnit;
            startAt = li + 1;
            found = true;
            break;
        }
        // Neither: a line before the list starts.
        ++r.entries;
        ++r.skipped;
    }
    if (!found) {
        std::string shown(trim(lines[0]).substr(0, 60));
        r.error = "no line starts with a frequency and no header names a frequency column (line 1: \"" +
                  shown + "\")";
        return r;
    }
    const auto col = [](const std::vector<std::string>& v, int c) -> std::string {
        return (c >= 0 && c < static_cast<int>(v.size())) ? v[static_cast<std::size_t>(c)] : std::string();
    };
    for (std::size_t li = startAt; li < lines.size(); ++li) {
        const std::vector<std::string> v = splitCsv(lines[li], sep);
        ++r.entries;
        double f = 0.0;
        std::string fs = col(v, cFreq);
        // A decimal comma, as a European Excel writes it in a ';' file.
        if (sep != ',' ) { std::replace(fs.begin(), fs.end(), ',', '.'); }
        if (!parseNumber(fs, f) || f <= 0.0) {
            ++r.skipped;
            continue;
        }
        const double hz = unit > 0.0 ? f * unit : (f < 100000.0 ? f * 1e6 : f);
        Bookmark b;
        b.freqHz = std::round(hz);
        b.name = col(v, cName);
        b.group = col(v, cGroup);
        b.mode = normaliseMode(col(v, cMode));
        double bw = 0.0;
        std::string bws = col(v, cBw);
        if (sep != ',') { std::replace(bws.begin(), bws.end(), ',', '.'); }
        b.bandwidthHz =
            (parseNumber(bws, bw) && bw > 0.0) ? bw * bwUnit : defaultBandwidthForMode(b.mode);
        const std::string fav = upper(col(v, cFav));
        b.favourite = fav == "TRUE" || fav == "YES" || fav == "1" || fav == "Y";
        if (b.name.empty()) {
            char def[32];
            std::snprintf(def, sizeof(def), "%.4f MHz", b.freqHz / 1e6);
            b.name = def;
        }
        r.items.push_back(std::move(b));
    }
    if (r.items.empty() && r.error.empty()) { r.error = "no line had a usable frequency"; }
    return r;
}

ImportResult importFrequencyList(std::string_view text) {
    if (text.find("<MemoryEntry") != std::string_view::npos) { return importSdrSharpXml(text); }
    return importCsv(text);
}

ImportResult importFrequencyFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ImportResult r;
        r.error = "cannot open \"" + path + "\"";
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    return importFrequencyList(text);
}

std::string exportSdrSharpXml(const std::vector<Bookmark>& list) {
    std::string out;
    out.reserve(list.size() * 320 + 256);
    out += "<?xml version=\"1.0\"?>\r\n";
    out += "<ArrayOfMemoryEntry xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
           "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\r\n";
    char num[64];
    for (const Bookmark& b : list) {
        out += "  <MemoryEntry>\r\n";
        out += std::string("    <IsFavourite>") + (b.favourite ? "true" : "false") + "</IsFavourite>\r\n";
        out += "    <Name>" + escapeXml(b.name) + "</Name>\r\n";
        out += "    <GroupName>" + escapeXml(b.group) + "</GroupName>\r\n";
        std::snprintf(num, sizeof(num), "%.0f", b.freqHz);
        out += std::string("    <Frequency>") + num + "</Frequency>\r\n";
        out += "    <DetectorType>" + escapeXml(b.mode) + "</DetectorType>\r\n";
        out += "    <Shift>0</Shift>\r\n";
        std::snprintf(num, sizeof(num), "%.0f", b.bandwidthHz);
        out += std::string("    <FilterBandwidth>") + num + "</FilterBandwidth>\r\n";
        out += "    <CenterFrequency>0</CenterFrequency>\r\n";
        out += "  </MemoryEntry>\r\n";
    }
    out += "</ArrayOfMemoryEntry>\r\n";
    return out;
}

}  // namespace cascade::core
