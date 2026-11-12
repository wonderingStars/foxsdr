// sweep_history.cpp - see sweep_history.hpp for the three formats and the caps.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sweep_history.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace cascade::core {

namespace {

constexpr const char* kFormatName = "foxsdr-sweep-history";

HistoryLoad fail(std::size_t line, std::string msg) {
    HistoryLoad r;
    r.ok = false;
    r.line = line;
    r.error = std::move(msg);
    return r;
}

// Strict decimal number: digits, '.', '-', 'e', 'E', '+' only (so no "nan", "inf", hex), the
// whole token, finite.
bool parseNum(std::string_view t, double& v) {
    if (t.empty()) { return false; }
    for (char c : t) {
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')) {
            return false;
        }
    }
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    return r.ec == std::errc{} && r.ptr == t.data() + t.size() && std::isfinite(v);
}

bool parseCount(std::string_view t, std::size_t& v) {
    if (t.empty() || t.size() > 9) { return false; }
    std::size_t n = 0;
    for (char c : t) {
        if (c < '0' || c > '9') { return false; }
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    v = n;
    return true;
}

std::string fmtDouble(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    return b;
}
std::string fmtLevel(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
    return b;
}

// Why a history may not be written, or "".
std::string historyProblem(const SweepHistory& h) {
    if (h.sweeps.empty()) { return "no sweeps"; }
    if (h.sweeps.size() > kHistoryMaxSweeps) { return "too many sweeps"; }
    std::size_t total = 0;
    for (const SweepLine& l : h.sweeps) {
        const std::string p = sweepLineProblem(l);
        if (!p.empty()) { return p; }
        total += l.db.size();
        if (total > kHistoryMaxTotalPoints) { return "too many points in all"; }
    }
    return {};
}

// Add a sweep to a load, checking the caps. Returns an error HistoryLoad or ok.
bool addSweep(HistoryLoad& r, SweepLine&& l, std::size_t line, std::size_t& total) {
    const std::string p = sweepLineProblem(l);
    if (!p.empty()) { r = fail(line, p); return false; }
    if (r.history.sweeps.size() >= kHistoryMaxSweeps) { r = fail(line, "too many sweeps"); return false; }
    total += l.db.size();
    if (total > kHistoryMaxTotalPoints) { r = fail(line, "too many points in all"); return false; }
    r.history.sweeps.push_back(std::move(l));
    return true;
}

std::size_t lineOfByte(std::string_view text, std::size_t byte1) {
    const std::size_t end = std::min(byte1 == 0 ? std::size_t{0} : byte1 - 1, text.size());
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
}

// ------------------------------------------------------------------ CSV
std::string toCsv(const SweepHistory& h) {
    std::string out = "#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n";
    for (const SweepLine& l : h.sweeps) {
        out += fmtDouble(l.timeS) + "," + fmtDouble(l.startHz) + "," + fmtDouble(l.stopHz) + "," +
               std::to_string(l.db.size());
        for (float v : l.db) {
            out += ',';
            if (!std::isnan(v)) { out += fmtLevel(v); }
        }
        out += '\n';
    }
    return out;
}

// ------------------------------------------------------------------ JSON
std::string toJson(const SweepHistory& h) {
    std::string out = std::string("{\"format\":\"") + kFormatName + "\",\"version\":1,\"sweeps\":[\n";
    for (std::size_t i = 0; i < h.sweeps.size(); ++i) {
        const SweepLine& l = h.sweeps[i];
        out += "{\"time_s\":" + fmtDouble(l.timeS) + ",\"start_hz\":" + fmtDouble(l.startHz) +
               ",\"stop_hz\":" + fmtDouble(l.stopHz) + ",\"db\":[";
        for (std::size_t k = 0; k < l.db.size(); ++k) {
            if (k != 0) { out += ','; }
            out += std::isnan(l.db[k]) ? std::string("null") : fmtLevel(l.db[k]);
        }
        out += i + 1 < h.sweeps.size() ? "]},\n" : "]}\n";
    }
    out += "]}\n";
    return out;
}

// Line on which each sweep object starts (the '{' that opens depth 3: root object 1, the
// "sweeps" array 2, a sweep 3), by a string- and escape-aware scan.
std::vector<std::size_t> sweepLines(std::string_view s) {
    std::vector<std::size_t> out;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    std::size_t line = 1;
    for (char c : s) {
        if (c == '\n') { ++line; }
        if (inStr) {
            if (esc) { esc = false; } else if (c == '\\') { esc = true; } else if (c == '"') { inStr = false; }
            continue;
        }
        if (c == '"') { inStr = true; }
        else if (c == '{' || c == '[') {
            ++depth;
            if (c == '{' && depth == 3) { out.push_back(line); }
        } else if (c == '}' || c == ']') { --depth; }
    }
    return out;
}

}  // namespace

std::string historyToText(const SweepHistory& h, HistoryFormat f, std::string* whyNot) {
    const std::string p = historyProblem(h);
    if (!p.empty()) {
        if (whyNot != nullptr) { *whyNot = p; }
        return {};
    }
    std::string out;
    switch (f) {
        case HistoryFormat::Csv: out = toCsv(h); break;
        case HistoryFormat::Json: out = toJson(h); break;
        case HistoryFormat::Xml: {
            out = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sweep-history format=\"") +
                  kFormatName + "\" version=\"1\">\n";
            for (const SweepLine& l : h.sweeps) {
                out += "<sweep time_s=\"" + fmtDouble(l.timeS) + "\" start_hz=\"" + fmtDouble(l.startHz) +
                       "\" stop_hz=\"" + fmtDouble(l.stopHz) + "\" points=\"" + std::to_string(l.db.size()) + "\">";
                for (std::size_t k = 0; k < l.db.size(); ++k) {
                    if (k != 0) { out += ' '; }
                    out += std::isnan(l.db[k]) ? std::string("nan") : fmtLevel(l.db[k]);
                }
                out += "</sweep>\n";
            }
            out += "</sweep-history>\n";
            break;
        }
    }
    if (out.size() > kHistoryMaxBytes) {
        if (whyNot != nullptr) { *whyNot = "file would exceed the size cap"; }
        return {};
    }
    return out;
}

// ------------------------------------------------------------------ CSV reader
HistoryLoad historyFromCsv(std::string_view text) {
    if (text.size() > kHistoryMaxBytes) { return fail(0, "file too large"); }
    HistoryLoad r;
    std::size_t lineNo = 0;
    std::size_t total = 0;
    std::size_t pos = 0;
    bool sawMagic = false;
    bool sawHeader = false;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) { nl = text.size(); }
        std::string_view ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (!ln.empty() && ln.back() == '\r') { ln.remove_suffix(1); }
        if (!sawMagic) {
            if (ln != "#foxsdr-sweep-history,csv,1") {
                return fail(lineNo, ln.rfind("#foxsdr-sweep-history,csv,", 0) == 0 ? "unsupported version"
                                                                                     : "not a sweep history file");
            }
            sawMagic = true;
            continue;
        }
        if (!sawHeader) {
            if (ln != "time_s,start_hz,stop_hz,points,db") { return fail(lineNo, "bad column header"); }
            sawHeader = true;
            continue;
        }
        if (ln.empty()) { continue; }
        // split on commas, keeping empties
        std::vector<std::string_view> f;
        std::size_t a = 0;
        while (true) {
            const std::size_t c = ln.find(',', a);
            if (c == std::string_view::npos) { f.push_back(ln.substr(a)); break; }
            f.push_back(ln.substr(a, c - a));
            a = c + 1;
            if (f.size() > kSweepMaxPointsPerLine + 4) { return fail(lineNo, "too many fields"); }
        }
        if (f.size() < 5) { return fail(lineNo, "fewer than 5 fields"); }
        SweepLine l;
        std::size_t n = 0;
        if (!parseNum(f[0], l.timeS)) { return fail(lineNo, "bad time_s"); }
        if (!parseNum(f[1], l.startHz)) { return fail(lineNo, "bad start_hz"); }
        if (!parseNum(f[2], l.stopHz)) { return fail(lineNo, "bad stop_hz"); }
        if (!parseCount(f[3], n) || n < 1 || n > kSweepMaxPointsPerLine) { return fail(lineNo, "bad points"); }
        if (f.size() != 4 + n) { return fail(lineNo, "field count does not match points"); }
        l.db.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string_view t = f[4 + i];
            if (t.empty()) { l.db.push_back(std::nanf("")); continue; }
            double v = 0;
            if (!parseNum(t, v)) { return fail(lineNo, "bad level in field " + std::to_string(5 + i)); }
            l.db.push_back(static_cast<float>(v));
        }
        if (!addSweep(r, std::move(l), lineNo, total)) { return r; }
    }
    if (!sawMagic) { return fail(1, "empty file"); }
    if (!sawHeader) { return fail(lineNo, "missing column header"); }
    r.ok = true;
    return r;
}

// ------------------------------------------------------------------ JSON reader
HistoryLoad historyFromJson(std::string_view text) {
    if (text.size() > kHistoryMaxBytes) { return fail(0, "file too large"); }
    using nlohmann::json;
    json doc;
    try {
        doc = json::parse(text.begin(), text.end());
    } catch (const json::parse_error& e) {
        return fail(lineOfByte(text, e.byte), "JSON syntax error");
    } catch (const std::exception&) {
        return fail(1, "JSON could not be read");
    }
    if (!doc.is_object()) { return fail(1, "top level is not an object"); }
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() != "format" && it.key() != "version" && it.key() != "sweeps") {
            return fail(1, "unknown key at top level");
        }
    }
    if (!doc.contains("format") || !doc["format"].is_string() || doc["format"].get<std::string>() != kFormatName) {
        return fail(1, "not a sweep history file");
    }
    if (!doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"].get<long long>() != kHistoryVersion) {
        return fail(1, "unsupported version");
    }
    if (!doc.contains("sweeps") || !doc["sweeps"].is_array()) { return fail(1, "sweeps is not an array"); }
    const std::vector<std::size_t> lines = sweepLines(text);
    HistoryLoad r;
    std::size_t total = 0;
    const json& arr = doc["sweeps"];
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const std::size_t ln = i < lines.size() ? lines[i] : 1;
        const json& s = arr[i];
        if (!s.is_object()) { return fail(ln, "sweep is not an object"); }
        for (auto it = s.begin(); it != s.end(); ++it) {
            const std::string& k = it.key();
            if (k != "time_s" && k != "start_hz" && k != "stop_hz" && k != "db") {
                return fail(ln, "unknown key in sweep");
            }
        }
        for (const char* k : {"time_s", "start_hz", "stop_hz"}) {
            if (!s.contains(k) || !s[k].is_number()) { return fail(ln, std::string("missing or bad ") + k); }
        }
        if (!s.contains("db") || !s["db"].is_array()) { return fail(ln, "db is not an array"); }
        const json& d = s["db"];
        if (d.empty() || d.size() > kSweepMaxPointsPerLine) { return fail(ln, "bad number of levels"); }
        SweepLine l;
        l.timeS = s["time_s"].get<double>();
        l.startHz = s["start_hz"].get<double>();
        l.stopHz = s["stop_hz"].get<double>();
        l.db.reserve(d.size());
        for (const json& v : d) {
            if (v.is_null()) { l.db.push_back(std::nanf("")); }
            else if (v.is_number()) { l.db.push_back(static_cast<float>(v.get<double>())); }
            else { return fail(ln, "a level is not a number"); }
        }
        if (!addSweep(r, std::move(l), ln, total)) { return r; }
    }
    if (r.history.sweeps.empty()) { return fail(1, "no sweeps"); }
    r.ok = true;
    return r;
}

// ------------------------------------------------------------------ XML reader
namespace {

struct Cursor {
    std::string_view s;
    std::size_t p = 0;
    std::size_t line = 1;
    bool eof() const { return p >= s.size(); }
    char peek() const { return eof() ? '\0' : s[p]; }
    void adv() {
        if (!eof()) {
            if (s[p] == '\n') { ++line; }
            ++p;
        }
    }
    void ws() { while (!eof() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) { adv(); } }
    bool lit(std::string_view t) {
        if (s.compare(p, t.size(), t) != 0) { return false; }
        for (std::size_t i = 0; i < t.size(); ++i) { adv(); }
        return true;
    }
};

// name="value" pairs up to '>' (consumed). Returns false with *why on a syntax problem.
bool readAttrs(Cursor& c, std::vector<std::pair<std::string, std::string>>& out, const char** why) {
    while (true) {
        c.ws();
        if (c.peek() == '>') { c.adv(); return true; }
        if (c.eof()) { *why = "unexpected end inside a tag"; return false; }
        std::string name;
        while (!c.eof() && ((c.peek() >= 'a' && c.peek() <= 'z') || c.peek() == '_' || c.peek() == '-')) {
            name += c.peek();
            c.adv();
            if (name.size() > 32) { *why = "attribute name too long"; return false; }
        }
        if (name.empty()) { *why = "expected an attribute"; return false; }
        if (c.peek() != '=') { *why = "expected ="; return false; }
        c.adv();
        if (c.peek() != '"') { *why = "expected a quoted value"; return false; }
        c.adv();
        std::string val;
        while (!c.eof() && c.peek() != '"') {
            if (c.peek() == '<' || c.peek() == '&') { *why = "markup in a value"; return false; }
            val += c.peek();
            c.adv();
            if (val.size() > 64) { *why = "attribute value too long"; return false; }
        }
        if (c.eof()) { *why = "unterminated value"; return false; }
        c.adv();
        for (const auto& kv : out) {
            if (kv.first == name) { *why = "duplicate attribute"; return false; }
        }
        out.emplace_back(std::move(name), std::move(val));
    }
}

const std::string* attr(const std::vector<std::pair<std::string, std::string>>& a, const char* k) {
    for (const auto& kv : a) { if (kv.first == k) { return &kv.second; } }
    return nullptr;
}

}  // namespace

HistoryLoad historyFromXml(std::string_view text) {
    if (text.size() > kHistoryMaxBytes) { return fail(0, "file too large"); }
    Cursor c{text};
    const char* why = nullptr;
    c.ws();
    if (!c.lit("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")) { return fail(c.line, "missing XML declaration"); }
    c.ws();
    if (!c.lit("<sweep-history")) { return fail(c.line, "root element is not sweep-history"); }
    std::vector<std::pair<std::string, std::string>> ra;
    if (!readAttrs(c, ra, &why)) { return fail(c.line, why); }
    if (ra.size() != 2 || attr(ra, "format") == nullptr || *attr(ra, "format") != kFormatName ||
        attr(ra, "version") == nullptr) {
        return fail(c.line, "root attributes are not format and version");
    }
    if (*attr(ra, "version") != "1") { return fail(c.line, "unsupported version"); }
    HistoryLoad r;
    std::size_t total = 0;
    while (true) {
        c.ws();
        if (c.eof()) { return fail(c.line, "missing </sweep-history>"); }
        if (c.lit("</sweep-history>")) { break; }
        const std::size_t startLine = c.line;
        if (!c.lit("<sweep")) { return fail(c.line, "expected <sweep> or the end of the file"); }
        std::vector<std::pair<std::string, std::string>> a;
        if (!readAttrs(c, a, &why)) { return fail(c.line, why); }
        if (a.size() != 4) { return fail(startLine, "a sweep needs exactly time_s, start_hz, stop_hz, points"); }
        SweepLine l;
        std::size_t n = 0;
        const std::string* t = attr(a, "time_s");
        const std::string* f0 = attr(a, "start_hz");
        const std::string* f1 = attr(a, "stop_hz");
        const std::string* pn = attr(a, "points");
        if (t == nullptr || f0 == nullptr || f1 == nullptr || pn == nullptr) {
            return fail(startLine, "a sweep needs exactly time_s, start_hz, stop_hz, points");
        }
        if (!parseNum(*t, l.timeS)) { return fail(startLine, "bad time_s"); }
        if (!parseNum(*f0, l.startHz)) { return fail(startLine, "bad start_hz"); }
        if (!parseNum(*f1, l.stopHz)) { return fail(startLine, "bad stop_hz"); }
        if (!parseCount(*pn, n) || n < 1 || n > kSweepMaxPointsPerLine) { return fail(startLine, "bad points"); }
        l.db.reserve(n);
        // text up to '<'
        while (true) {
            c.ws();
            if (c.peek() == '<') { break; }
            if (c.eof()) { return fail(c.line, "unexpected end inside a sweep"); }
            std::size_t b = c.p;
            while (!c.eof() && c.peek() != ' ' && c.peek() != '\t' && c.peek() != '\n' && c.peek() != '\r' &&
                   c.peek() != '<') {
                if (c.peek() == '&') { return fail(c.line, "entities are not allowed"); }
                c.adv();
            }
            const std::string_view tok = text.substr(b, c.p - b);
            if (l.db.size() >= n) { return fail(startLine, "more levels than points"); }
            if (tok == "nan") { l.db.push_back(std::nanf("")); continue; }
            double v = 0;
            if (!parseNum(tok, v)) { return fail(c.line, "bad level"); }
            l.db.push_back(static_cast<float>(v));
        }
        if (!c.lit("</sweep>")) { return fail(c.line, "expected </sweep>"); }
        if (l.db.size() != n) { return fail(startLine, "fewer levels than points"); }
        if (!addSweep(r, std::move(l), startLine, total)) { return r; }
    }
    c.ws();
    if (!c.eof()) { return fail(c.line, "content after the end of the document"); }
    if (r.history.sweeps.empty()) { return fail(1, "no sweeps"); }
    r.ok = true;
    return r;
}

HistoryLoad historyFromText(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) { ++i; }
    if (i >= text.size()) { return fail(1, "empty file"); }
    switch (text[i]) {
        case '#': return historyFromCsv(text);
        case '{': return historyFromJson(text);
        case '<': return historyFromXml(text);
        default: return fail(1, "not a sweep history file");
    }
}

// ------------------------------------------------------------------ pairs import
HistoryLoad importFreqLevelPairs(std::string_view text, const PairsOptions& opt) {
    if (text.size() > kHistoryMaxBytes) { return fail(0, "file too large"); }
    if (!(opt.freqUnitHz > 0.0) || !std::isfinite(opt.freqUnitHz) || !(opt.maxSpacingDeviation >= 0.0)) {
        return fail(0, "bad options");
    }
    std::vector<double> hz;
    std::vector<float> lv;
    std::vector<std::size_t> lines;
    std::size_t lineNo = 0;
    std::size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) { nl = text.size(); }
        std::string_view ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) { ln.remove_suffix(1); }
        while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\t')) { ln.remove_prefix(1); }
        if (ln.empty()) { continue; }
        if (first) {
            first = false;
            const char c0 = ln.front();
            if (c0 == '#' || (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) { continue; }
        }
        std::vector<std::string_view> f;
        std::size_t a = 0;
        while (a < ln.size()) {
            while (a < ln.size() && (ln[a] == ',' || ln[a] == ';' || ln[a] == '\t' || ln[a] == ' ')) { ++a; }
            if (a >= ln.size()) { break; }
            std::size_t b = a;
            while (b < ln.size() && ln[b] != ',' && ln[b] != ';' && ln[b] != '\t' && ln[b] != ' ') { ++b; }
            f.push_back(ln.substr(a, b - a));
            a = b;
            if (f.size() > 3) { break; }
        }
        if (f.size() != 2) { return fail(lineNo, "expected exactly two values (frequency, level)"); }
        double fr = 0, db = 0;
        if (!parseNum(f[0], fr) || fr < 0.0) { return fail(lineNo, "bad frequency"); }
        if (!parseNum(f[1], db)) { return fail(lineNo, "bad level"); }
        if (db < kSweepMinLevelDb || db > kSweepMaxLevelDb) { return fail(lineNo, "level out of range"); }
        if (hz.size() >= kSweepMaxPointsPerLine) { return fail(lineNo, "too many points"); }
        hz.push_back(fr * opt.freqUnitHz);
        lv.push_back(static_cast<float>(db));
        lines.push_back(lineNo);
        if (hz.size() >= 2 && !(hz.back() > hz[hz.size() - 2])) {
            return fail(lineNo, "frequencies are not strictly increasing");
        }
    }
    if (hz.size() < 2) { return fail(lineNo == 0 ? 1 : lineNo, "fewer than two pairs"); }
    const double step = (hz.back() - hz.front()) / static_cast<double>(hz.size() - 1);
    for (std::size_t i = 1; i < hz.size(); ++i) {
        if (std::fabs((hz[i] - hz[i - 1]) - step) > opt.maxSpacingDeviation * step) {
            return fail(lines[i], "frequencies are not evenly spaced");
        }
    }
    HistoryLoad r;
    SweepLine l;
    l.startHz = hz.front();
    l.stopHz = hz.back();
    l.db = std::move(lv);
    std::size_t total = 0;
    if (!addSweep(r, std::move(l), lines.front(), total)) { return r; }
    r.ok = true;
    return r;
}

}  // namespace cascade::core
