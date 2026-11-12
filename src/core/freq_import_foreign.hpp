// freq_import_foreign - bookmark import from gqrx (bookmarks.csv) and CubicSDR (bookmarks.xml), chosen by content.
//
// STANDALONE, header-only, a NEW file beside the web lane's importers (feat/web-bookmarks: core/freq_import_formats.cpp, CHIRP CSV and
// SDR++ JSON). It does not touch core/freq_import.* and has its own result struct (ForeignImportResult) with the same fields the web
// lane's ImportResult carries (items, entries, skipped, modeDefaulted, format, error); the wiring lane folds the two (hand-off in
// docs/feature-records/core-formats.md). Bookmark is core/bookmark.hpp's.
//
// *** BOTH FORMATS ARE UNVERIFIED. *** No real gqrx or CubicSDR file has been seen and no format document was found:
//  * gqrx: the web search of 2026-11-11 found no official description of bookmarks.csv. What is implemented comes from the task's
//    description and the writer's memory of the file: semicolon-separated lines "Frequency ; Name ; Modulation ; Bandwidth ; Tags"
//    (frequency in hertz, the file starts with a "# Tag name ; color" block and then a "# Frequency ; Name ; ..." header comment,
//    lines starting with # are comments), modulation names "Narrow FM", "WFM (mono)", "WFM (stereo)", "WFM (oirt)", "AM", "AM-Sync",
//    "LSB", "USB", "CW-L", "CW-U", "Raw I/Q", "Demod Off". The reader is tolerant: ';', tab or ',' separated, a header comment (if
//    present) decides the column order, otherwise the order above. One search result (a third-party tool's sample) showed
//    comma-separated "frequency, mode, name" lines; that layout is NOT assumed here (the header decides when there is one).
//  * CubicSDR: the search found only release notes saying bookmarks live in a per-user bookmarks.xml (with .backup and .lastload
//    copies); no schema. What is implemented is the writer's memory: a <cubicsdr_bookmarks> root, <group name="..."> elements holding
//    <modem> elements whose children include <frequency> (Hz), <bandwidth> (Hz), <type> (FM, NBFM, WBFM/FMS, AM, LSB, USB, DSB, I/Q)
//    and <user_label> (also read: <label>, <name>); an element named <bookmark> is read the same way. Range bookmarks (<range>) and
//    recents are not bookmarks and are ignored.
//  * PERSEUS: NOT BUILT. Searched (2026-11-11): "Perseus SDR software frequency memory bookmark file format description" found no
//    description of the Perseus program's own memory file (only station lists made FOR Perseus, EiBi / MWList CSV, and SDR Console's
//    XML memories). Without a description or a real file nothing can be written without inventing a format.
//
// HOSTILE INPUT. Refused: more than 16 MiB, a NUL byte, an XML DOCTYPE or ENTITY declaration (no entity expansion is ever done beyond
// the five predefined entities and numeric references), element nesting over 64 deep, more than 200000 entries, a line over 4096
// bytes (gqrx), a frequency outside (0, 1e12] Hz. Names are cut to 128 bytes on a UTF-8 boundary, control characters removed, invalid
// UTF-8 bytes replaced by '?'. Nothing here allocates unboundedly from the input's own claims.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/bookmark.hpp"

namespace cascade::core {

struct ForeignImportResult {
    std::vector<Bookmark> items;
    std::size_t entries = 0;        // entries seen
    std::size_t skipped = 0;        // seen but unusable (no or bad frequency)
    std::size_t modeDefaulted = 0;  // a mode FoxSDR has no name for: imported as NFM
    std::string format;             // "gqrx CSV" or "CubicSDR XML"
    std::string error;              // set when nothing could be read
};

namespace foreign {

constexpr std::size_t kMaxBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxEntries = 200000;
constexpr std::size_t kMaxLine = 4096;
constexpr std::size_t kMaxName = 128;
constexpr int kMaxDepth = 64;

inline double defaultBandwidth(const std::string& mode) {
    if (mode == "WFM" || mode == "RAW") { return 150000.0; }
    if (mode == "AM") { return 10000.0; }
    if (mode == "DSB") { return 6000.0; }
    if (mode == "USB" || mode == "LSB") { return 2800.0; }
    if (mode == "CW") { return 500.0; }
    return 12500.0;
}

inline std::string lowerTrim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) { s.remove_prefix(1); }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) { s.remove_suffix(1); }
    std::string o(s);
    for (char& c : o) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return o;
}

inline std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) { s.remove_prefix(1); }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) { s.remove_suffix(1); }
    return std::string(s);
}

// A name safe to show and save: valid UTF-8 only (bad bytes become '?'), no control characters, at most kMaxName bytes.
inline std::string cleanName(std::string_view s) {
    std::string o;
    std::size_t i = 0;
    while (i < s.size() && o.size() < kMaxName) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        bool ok = true;
        if (c >= 0x80) {
            std::size_t need = 0;
            std::uint32_t cp = 0, minCp = 0;
            if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; minCp = 0x80; }
            else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; minCp = 0x800; }
            else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; minCp = 0x10000; }
            else { ok = false; }
            if (ok && i + need >= s.size()) { ok = false; }
            for (std::size_t k = 1; ok && k <= need; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (ok && (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) { ok = false; }
            if (ok) { len = need + 1; }
        }
        if (!ok) { o.push_back('?'); ++i; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7F)) { ++i; continue; }
        if (o.size() + len > kMaxName) { break; }
        o.append(s.substr(i, len));
        i += len;
    }
    return trimmed(o);
}

inline bool parseHz(std::string_view s, double* out) {
    const std::string t = trimmed(s);
    if (t.empty() || t.size() > 24) { return false; }
    if (t.find_first_not_of("0123456789.+-eE") != std::string::npos) { return false; }   // no hex, inf or nan
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v) || v <= 0.0 || v > 1.0e12) { return false; }
    *out = v;
    return true;
}

inline std::string defaultName(double hz) {
    char b[40];
    std::snprintf(b, sizeof b, "%.4f MHz", hz / 1e6);
    return b;
}

inline std::string_view dropBom(std::string_view t) {
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB && static_cast<unsigned char>(t[2]) == 0xBF) { t.remove_prefix(3); }
    return t;
}

inline bool hasNul(std::string_view t) { return t.find('\0') != std::string_view::npos; }

inline std::string gqrxMode(const std::string& raw, bool* known) {
    const std::string m = lowerTrim(raw);
    *known = true;
    if (m == "narrow fm" || m == "nfm" || m == "fm") { return "NFM"; }
    if (m == "wfm (mono)" || m == "wfm (stereo)" || m == "wfm (oirt)" || m == "wfm") { return "WFM"; }
    if (m == "am" || m == "am-sync") { return "AM"; }
    if (m == "lsb") { return "LSB"; }
    if (m == "usb") { return "USB"; }
    if (m == "cw-l" || m == "cw-u" || m == "cw") { return "CW"; }
    if (m == "raw i/q" || m == "demod off" || m == "raw") { return "RAW"; }
    *known = false;
    return "NFM";
}

inline std::string cubicMode(const std::string& raw, bool* known) {
    const std::string m = lowerTrim(raw);
    *known = true;
    if (m == "fm" || m == "nfm" || m == "nbfm") { return "NFM"; }
    if (m == "wbfm" || m == "wfm" || m == "fms") { return "WFM"; }
    if (m == "am") { return "AM"; }
    if (m == "lsb") { return "LSB"; }
    if (m == "usb") { return "USB"; }
    if (m == "dsb") { return "DSB"; }
    if (m == "cw") { return "CW"; }
    if (m == "i/q" || m == "iq" || m == "raw") { return "RAW"; }
    *known = false;
    return "NFM";
}

// ---- gqrx ---------------------------------------------------------------------------------------------------------------------

inline std::vector<std::string> splitFields(std::string_view line, char sep) {
    std::vector<std::string> f;
    std::size_t s = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == sep) { f.push_back(trimmed(line.substr(s, i - s))); s = i + 1; }
    }
    return f;
}

// The separator a line uses: ';' if present, else tab, else ','.
inline char separatorOf(std::string_view line) {
    if (line.find(';') != std::string_view::npos) { return ';'; }
    if (line.find('\t') != std::string_view::npos) { return '\t'; }
    return ',';
}

struct GqrxColumns { int freq = 0, name = 1, mode = 2, bw = 3, tags = 4; };

inline bool isColourName(const std::string& s) { return !s.empty() && s[0] == '#' && s.size() <= 9; }

inline bool looksLikeGqrx(std::string_view text) {
    std::size_t pos = 0;
    int lines = 0;
    while (pos < text.size() && lines < 200) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) { e = text.size(); }
        std::string_view ln = text.substr(pos, e - pos);
        pos = e + 1;
        ++lines;
        const std::string t = trimmed(ln);
        if (t.empty()) { continue; }
        if (t[0] == '#') {
            const std::string l = lowerTrim(t);
            if (l.find("frequency") != std::string::npos && (l.find("modulation") != std::string::npos || l.find("name") != std::string::npos)) { return true; }
            continue;
        }
        // first data line: ';'-separated with a numeric first field and at least 3 fields (the tag-colour block before it is skipped)
        const auto f = splitFields(t, separatorOf(t));
        if (f.size() >= 2 && isColourName(f[1])) { continue; }
        double hz = 0;
        return f.size() >= 3 && parseHz(f[0], &hz) && t.find(';') != std::string::npos;
    }
    return false;
}

inline ForeignImportResult importGqrxCsv(std::string_view text) {
    ForeignImportResult r;
    r.format = "gqrx CSV";
    text = dropBom(text);
    if (text.size() > kMaxBytes) { r.error = "the file is larger than 16 MiB"; return r; }
    if (hasNul(text)) { r.error = "not a text file (it contains a NUL byte)"; return r; }
    if (trimmed(text).empty()) { r.error = "the file is empty"; return r; }
    GqrxColumns col;
    bool seenColumns = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) { e = text.size(); }
        std::string_view ln = text.substr(pos, e - pos);
        pos = e + 1;
        if (ln.size() > kMaxLine) { ++r.entries; ++r.skipped; continue; }
        const std::string t = trimmed(ln);
        if (t.empty()) { continue; }
        if (t[0] == '#') {
            // a header comment naming the columns decides their order (first one wins)
            std::string body = t.substr(1);
            const auto f = splitFields(body, separatorOf(body));
            if (!seenColumns && f.size() >= 3) {
                GqrxColumns c{-1, -1, -1, -1, -1};
                for (std::size_t i = 0; i < f.size(); ++i) {
                    const std::string h = lowerTrim(f[i]);
                    if (h.rfind("freq", 0) == 0) { c.freq = static_cast<int>(i); }
                    else if (h == "name") { c.name = static_cast<int>(i); }
                    else if (h.rfind("modulation", 0) == 0 || h == "mode") { c.mode = static_cast<int>(i); }
                    else if (h.rfind("bandwidth", 0) == 0) { c.bw = static_cast<int>(i); }
                    else if (h.rfind("tag", 0) == 0) { c.tags = static_cast<int>(i); }
                }
                if (c.freq >= 0 && (c.name >= 0 || c.mode >= 0)) { col = c; seenColumns = true; }
            }
            continue;
        }
        const auto f = splitFields(t, separatorOf(t));
        auto cell = [&](int i) -> std::string { return (i >= 0 && static_cast<std::size_t>(i) < f.size()) ? f[static_cast<std::size_t>(i)] : std::string(); };
        double hz = 0.0;
        if (!parseHz(cell(col.freq), &hz)) {
            // the tag-colour block ("Aircraft ; #ff0000") and stray text are not bookmarks: only a line with a number first is counted
            if (f.size() >= 2 && isColourName(f[1])) { continue; }
            if (f.empty() || !std::isdigit(static_cast<unsigned char>(f[0].empty() ? 'x' : f[0][0]))) { continue; }
            ++r.entries;
            ++r.skipped;
            continue;
        }
        if (r.entries >= kMaxEntries) { r.error = "more than 200000 entries: import stopped"; break; }
        ++r.entries;
        Bookmark b;
        b.freqHz = std::round(hz);
        b.name = cleanName(cell(col.name));
        if (b.name.empty()) { b.name = defaultName(b.freqHz); }
        bool known = true;
        const std::string modeText = cell(col.mode);
        b.mode = gqrxMode(modeText, &known);
        if (!known && !trimmed(modeText).empty()) { ++r.modeDefaulted; }
        double bw = 0.0;
        b.bandwidthHz = (parseHz(cell(col.bw), &bw) && bw <= 1.0e8) ? bw : defaultBandwidth(b.mode);
        // gqrx tags: one or more separated by ','; the first becomes the group ("Untagged" is gqrx's "no tag")
        const std::string tags = cell(col.tags);
        const std::string first = trimmed(tags.substr(0, tags.find(',')));
        if (!first.empty() && lowerTrim(first) != "untagged") { b.group = cleanName(first); }
        r.items.push_back(std::move(b));
    }
    if (r.items.empty() && r.error.empty()) { r.error = "no line had a usable frequency"; }
    return r;
}

// ---- CubicSDR -------------------------------------------------------------------------------------------------------------------

inline void appendUtf8(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) { o.push_back(static_cast<char>(cp)); }
    else if (cp < 0x800) { o.push_back(static_cast<char>(0xC0 | (cp >> 6))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { o.push_back(static_cast<char>(0xE0 | (cp >> 12))); o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { o.push_back(static_cast<char>(0xF0 | (cp >> 18))); o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

// The five predefined entities and numeric character references; anything else stays as written.
inline std::string xmlUnescape(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o.push_back(s[i]); continue; }
        const std::size_t e = s.find(';', i);
        if (e == std::string_view::npos || e - i > 10) { o.push_back('&'); continue; }
        const std::string_view ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") { o.push_back('&'); }
        else if (ent == "lt") { o.push_back('<'); }
        else if (ent == "gt") { o.push_back('>'); }
        else if (ent == "quot") { o.push_back('"'); }
        else if (ent == "apos") { o.push_back('\''); }
        else if (ent.size() >= 2 && ent[0] == '#') {
            std::uint32_t cp = 0;
            bool ok = true;
            if (ent[1] == 'x' || ent[1] == 'X') {
                if (ent.size() < 3) { ok = false; }
                for (std::size_t k = 2; ok && k < ent.size(); ++k) { if (!std::isxdigit(static_cast<unsigned char>(ent[k]))) { ok = false; } else { cp = cp * 16 + static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(ent[k])) ? ent[k] - '0' : (std::tolower(static_cast<unsigned char>(ent[k])) - 'a' + 10)); } }
            } else {
                for (std::size_t k = 1; ok && k < ent.size(); ++k) { if (!std::isdigit(static_cast<unsigned char>(ent[k]))) { ok = false; } else { cp = cp * 10 + static_cast<std::uint32_t>(ent[k] - '0'); } }
            }
            if (!ok || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { o.push_back('?'); } else { appendUtf8(o, cp); }
        } else { o.append(s.substr(i, e - i + 1)); }
        i = e;
    }
    return o;
}

inline bool looksLikeCubicSdr(std::string_view text) {
    std::string head(text.substr(0, std::min<std::size_t>(text.size(), 4096)));
    for (char& c : head) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return head.find("<cubicsdr_bookmarks") != std::string::npos;
}

inline ForeignImportResult importCubicSdrXml(std::string_view text) {
    ForeignImportResult r;
    r.format = "CubicSDR XML";
    text = dropBom(text);
    if (text.size() > kMaxBytes) { r.error = "the file is larger than 16 MiB"; return r; }
    if (hasNul(text)) { r.error = "not a text file (it contains a NUL byte)"; return r; }
    if (trimmed(text).empty()) { r.error = "the file is empty"; return r; }
    if (!looksLikeCubicSdr(text)) { r.error = "not a CubicSDR bookmarks file: no <cubicsdr_bookmarks> element"; return r; }
    {
        std::string lower(text);
        for (char& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        if (lower.find("<!entity") != std::string::npos || lower.find("<!doctype") != std::string::npos) {
            r.error = "refused: the file declares a DOCTYPE or an ENTITY (not part of a bookmarks file)";
            return r;
        }
    }
    struct Frame { std::string name; std::string groupName; };
    std::vector<Frame> stack;
    std::vector<std::string> groups;   // group names currently open
    bool inItem = false;
    int itemDepth = 0;
    std::string freq, bw, type, label;
    std::string curText;
    std::string curChild;
    std::size_t i = 0;
    bool malformed = false;
    while (i < text.size() && !malformed) {
        const std::size_t lt = text.find('<', i);
        if (lt == std::string_view::npos) { break; }
        if (inItem && !stack.empty()) { curText += std::string(text.substr(i, lt - i)); }
        if (text.compare(lt, 4, "<!--") == 0) {
            const std::size_t e = text.find("-->", lt + 4);
            if (e == std::string_view::npos) { malformed = true; break; }
            i = e + 3;
            continue;
        }
        if (text.compare(lt, 2, "<?") == 0) {
            const std::size_t e = text.find("?>", lt + 2);
            if (e == std::string_view::npos) { malformed = true; break; }
            i = e + 2;
            continue;
        }
        if (text.compare(lt, 9, "<![CDATA[") == 0) {
            const std::size_t e = text.find("]]>", lt + 9);
            if (e == std::string_view::npos) { malformed = true; break; }
            if (inItem) { curText += std::string(text.substr(lt + 9, e - lt - 9)); }
            i = e + 3;
            continue;
        }
        const std::size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos) { malformed = true; break; }
        std::string_view tag = text.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (!tag.empty() && tag[0] == '/') {
            const std::string n = lowerTrim(tag.substr(1));
            if (stack.empty() || stack.back().name != n) { malformed = true; break; }
            const Frame f = stack.back();
            stack.pop_back();
            if (n == "group") { if (!groups.empty()) { groups.pop_back(); } }
            if (inItem && static_cast<int>(stack.size()) == itemDepth) {
                // closing a direct child of the item: its text is the value
                const std::string v = xmlUnescape(trimmed(curText));
                if (n == "frequency") { freq = v; }
                else if (n == "bandwidth") { bw = v; }
                else if (n == "type") { type = v; }
                else if (n == "user_label" || (n == "label" && label.empty()) || (n == "name" && label.empty())) { if (!v.empty()) { label = v; } }
                curText.clear();
            } else if (inItem && static_cast<int>(stack.size()) < itemDepth) {
                // the item itself closed
                inItem = false;
                ++r.entries;
                double hz = 0.0;
                if (r.entries > kMaxEntries) { r.error = "more than 200000 entries: import stopped"; break; }
                if (!parseHz(freq, &hz)) {
                    ++r.skipped;
                } else {
                    Bookmark b;
                    b.freqHz = std::round(hz);
                    b.name = cleanName(label);
                    if (b.name.empty()) { b.name = defaultName(b.freqHz); }
                    bool known = true;
                    b.mode = cubicMode(type, &known);
                    if (!known && !trimmed(type).empty()) { ++r.modeDefaulted; }
                    double w = 0.0;
                    b.bandwidthHz = (parseHz(bw, &w) && w <= 1.0e8) ? w : defaultBandwidth(b.mode);
                    if (!groups.empty() && !groups.back().empty()) { b.group = groups.back(); }
                    r.items.push_back(std::move(b));
                }
            }
            (void)f;
            curText.clear();
            continue;
        }
        const bool selfClose = !tag.empty() && tag.back() == '/';
        if (selfClose) { tag.remove_suffix(1); }
        const std::size_t sp = tag.find_first_of(" \t\r\n");
        const std::string n = lowerTrim(tag.substr(0, sp));
        if (n.empty() || n[0] == '!') { malformed = true; break; }
        if (selfClose) {
            if (n == "group") { /* an empty group */ }
            continue;
        }
        if (static_cast<int>(stack.size()) >= kMaxDepth) { r.error = "refused: elements nested more than 64 deep"; return r; }
        Frame fr;
        fr.name = n;
        if (n == "group") {
            std::string gname;
            const std::string attrs(tag.substr(sp == std::string_view::npos ? tag.size() : sp));
            const std::size_t k = attrs.find("name=");
            if (k != std::string::npos && k + 5 < attrs.size() && (attrs[k + 5] == '"' || attrs[k + 5] == '\'')) {
                const char q = attrs[k + 5];
                const std::size_t e = attrs.find(q, k + 6);
                if (e != std::string::npos) { gname = cleanName(xmlUnescape(attrs.substr(k + 6, e - k - 6))); }
            }
            groups.push_back(gname);
        }
        stack.push_back(fr);
        if (!inItem && (n == "modem" || n == "bookmark")) {
            inItem = true;
            itemDepth = static_cast<int>(stack.size());
            freq.clear(); bw.clear(); type.clear(); label.clear();
        }
        curText.clear();
    }
    if (!malformed && !stack.empty()) { malformed = true; }   // the file ends inside an element: truncated
    if (malformed) {
        if (r.items.empty()) { r.error = "the XML is malformed (unbalanced or unterminated tags)"; }
        else { r.error = "the XML is malformed after " + std::to_string(r.items.size()) + " bookmarks: the rest was not read"; }
        return r;
    }
    if (r.error.empty() && r.items.empty()) { r.error = "the file has no bookmark with a usable frequency"; }
    return r;
}

}  // namespace foreign

// Either, chosen from the content: a <cubicsdr_bookmarks> element means CubicSDR, a gqrx-style header comment or ';'-separated numeric
// lines mean gqrx; anything else is refused with an error naming both formats.
inline ForeignImportResult importForeignBookmarks(std::string_view text) {
    text = foreign::dropBom(text);
    if (text.size() > foreign::kMaxBytes) { ForeignImportResult r; r.error = "the file is larger than 16 MiB"; return r; }
    if (foreign::hasNul(text)) { ForeignImportResult r; r.error = "not a text file (it contains a NUL byte)"; return r; }
    if (foreign::looksLikeCubicSdr(text)) { return foreign::importCubicSdrXml(text); }
    if (foreign::looksLikeGqrx(text)) { return foreign::importGqrxCsv(text); }
    ForeignImportResult r;
    r.error = foreign::trimmed(text).empty() ? "the file is empty" : "not a gqrx bookmarks.csv or a CubicSDR bookmarks.xml";
    return r;
}

// A file by UTF-8 path: the size is checked BEFORE reading (a 4 GB file is never loaded), a directory or missing file is an error.
inline ForeignImportResult importForeignFile(const std::string& path) {
    ForeignImportResult r;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::is_regular_file(p, ec)) { r.error = "cannot open " + path + " (not a file)"; return r; }
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) { r.error = "cannot read the size of " + path; return r; }
    if (size > foreign::kMaxBytes) { r.error = "the file is larger than 16 MiB"; return r; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { r.error = "cannot open " + path; return r; }
    std::string text(static_cast<std::size_t>(size), ' ');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    return importForeignBookmarks(text);
}

}  // namespace cascade::core
