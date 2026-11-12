// cabrillo.hpp - Cabrillo v3 log writer (hamops lane): header validated against the specification's value lists, QSO: lines per contest from
// a field table, chronological order, field truncation, CR LF line ends. Pure code: it produces text (or a file at a path the caller names);
// it has no transmit path and no network.
//
// SOURCES (opened 2026-11-11; full list in docs/feature-records/hamops.md):
//   * Header: https://wwrof.org/cabrillo/cabrillo-v3-header/  - START-OF-LOG: 3.0 first, END-OF-LOG: last; "TAG: value"; CONTEST only A-Z 0-9 hyphen,
//     at most 32 characters; the CATEGORY-* value lists; CLAIMED-SCORE "Integer only"; NAME at most 75 characters; ADDRESS lines at most 45
//     characters, up to 6; OPERATORS / SOAPBOX lines at most 75 characters with the tag; X-<anything> comment lines; "All QSO: lines must appear in
//     chronological order".
//   * QSO data: https://wwrof.org/cabrillo/cabrillo-qso-data/ - generic "QSO: freq mo date time call rst exch call rst exch t", date yyyy-mm-dd,
//     time nnnn, frequency kHz, mode CW/PH/FM/RY/DG, call "Only A-Z, 0-9, and /", t = transmitter 0 or 1.
//   * Notes: https://wwrof.org/cabrillo/cabrillo-specification-notes/ - platform line ending; data truncated to fit its field; NAME, ADDRESS, CLUB,
//     OPERATORS and SOAPBOX present even when empty; CLAIMED-SCORE only for the sponsor's reports. (That page says ADDRESS up to four lines, the
//     header page up to six: this writer accepts six and lists the difference in the record.)
//   * Per-contest templates actually opened: CQ WW (https://www.cqww.com/cabrillo.htm), CQ WPX (https://cqwpx.com/cabrillo.htm) and ARRL
//     Sweepstakes (https://www.arrl.org/cabrillo-format-tutorial). Column layout: the samples on the QSO-data page put the call at column 31, the
//     received call at column 56 and the transmitter column at 81; the fixed widths below reproduce those columns for the RST+exchange contests.
//     Sponsors accept any space delimiting, so columns are courtesy, not a rule.
//
// PRIVACY: callsigns and exchanges are the user's own log data. Messages in CabrilloResult::problems name a QSO by its index only, never its call.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "qso_log.hpp"

namespace cascade::core::ham {

enum class FieldKind { Rst, Number, Text };

struct ExchField {
    const char* key;   // the macro / session key: RST, NR, ZONE, PREC, CHECK, SECT
    int width;         // Cabrillo column width; longer data is truncated to fit
    FieldKind kind;
};

struct ContestDef {
    std::string id;            // the CONTEST: value
    std::string modeCategory;  // CATEGORY-MODE the contest id implies: CW or SSB
    std::vector<ExchField> exch;
    int callWidth = 13;
    const char* source = "";   // page the template was read from
};

// The contests whose Cabrillo pages were opened. The exchange field order is the order of the received/sent tokens in Qso.
inline const std::vector<ContestDef>& cabrilloContests() {
    static const std::vector<ContestDef> k = [] {
        std::vector<ContestDef> v;
        const std::vector<ExchField> cqww = {{"RST", 3, FieldKind::Rst}, {"ZONE", 6, FieldKind::Number}};
        const std::vector<ExchField> wpx = {{"RST", 3, FieldKind::Rst}, {"NR", 6, FieldKind::Number}};
        // Sweepstakes: serial, precedence (one letter), check (two digits), ARRL section; no RST.
        const std::vector<ExchField> ss = {{"NR", 4, FieldKind::Number}, {"PREC", 1, FieldKind::Text}, {"CHECK", 2, FieldKind::Number},
                                           {"SECT", 3, FieldKind::Text}};
        v.push_back({"CQ-WW-CW", "CW", cqww, 13, "https://www.cqww.com/cabrillo.htm"});
        v.push_back({"CQ-WW-SSB", "SSB", cqww, 13, "https://www.cqww.com/cabrillo.htm"});
        v.push_back({"CQ-WPX-CW", "CW", wpx, 13, "https://cqwpx.com/cabrillo.htm"});
        v.push_back({"CQ-WPX-SSB", "SSB", wpx, 13, "https://cqwpx.com/cabrillo.htm"});
        v.push_back({"ARRL-SS-CW", "CW", ss, 13, "https://www.arrl.org/cabrillo-format-tutorial"});
        v.push_back({"ARRL-SS-SSB", "SSB", ss, 13, "https://www.arrl.org/cabrillo-format-tutorial"});
        return v;
    }();
    return k;
}

inline const ContestDef* findCabrilloContest(const std::string& id) {
    for (const ContestDef& c : cabrilloContests()) {
        if (c.id == id) { return &c; }
    }
    return nullptr;
}

struct CabrilloHeader {
    std::string contest;               // e.g. CQ-WW-CW (required, must be a table contest to write QSO lines)
    std::string callsign;              // required
    std::string location;              // ARRL section / state / DX
    std::string categoryAssisted, categoryBand, categoryMode, categoryOperator, categoryOverlay, categoryPower, categoryStation, categoryTime,
        categoryTransmitter;
    long long claimedScore = -1;       // -1 = omit
    std::string club, createdBy, email, gridLocator, name;
    std::vector<std::string> address;  // lines, at most 6 of at most 45 characters
    std::vector<std::string> operators;// callsigns; the host call prefixed with @ by the caller
    std::vector<std::string> soapbox;  // lines of at most 75 characters minus the tag
    std::vector<std::string> comments; // written as X-<text> lines
};

struct CabrilloResult {
    bool ok = false;                    // false: the header or contest was refused, nothing usable in `text`
    std::string text;
    int qsoLines = 0;
    int skipped = 0;
    std::vector<std::string> problems;  // sentences, QSO named by index only
};

namespace cabrillo_detail {

inline bool oneOf(const std::string& v, std::initializer_list<const char*> list) {
    for (const char* s : list) {
        if (v == s) { return true; }
    }
    return false;
}

// Replaces anything that is not printable ASCII (including line breaks that would break a tag line) by a space.
inline std::string cleanText(const std::string& s, std::size_t maxLen) {
    std::string r;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        r.push_back((u >= 32 && u < 127) ? c : ' ');
        if (r.size() >= maxLen) { break; }
    }
    return r;
}

inline std::string pad(std::string s, int width, bool rightAlign) {
    if (static_cast<int>(s.size()) > width) { s.resize(static_cast<std::size_t>(width)); }
    const std::size_t padN = static_cast<std::size_t>(width) - s.size();
    return rightAlign ? std::string(padN, ' ') + s : s + std::string(padN, ' ');
}

inline bool validToken(const std::string& t, FieldKind kind) {
    if (t.empty() || t.size() > 12) { return false; }
    for (char c : t) {
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (kind == FieldKind::Text ? !(digit || alpha) : !digit) { return false; }
    }
    if (kind == FieldKind::Rst && (t.size() < 2 || t.size() > 3)) { return false; }
    return true;
}

inline void addTag(std::string& out, const char* tag, const std::string& value, const char* eol) {
    out += tag;
    out += ':';
    if (!value.empty()) { out += ' '; out += value; }
    out += eol;
}

}  // namespace cabrillo_detail

// Writes the whole log. `qsos` need not be sorted: they are written in chronological order (stable for equal times).
inline CabrilloResult writeCabrillo(const CabrilloHeader& h, std::vector<Qso> qsos, const char* eol = "\r\n") {
    using namespace cabrillo_detail;
    CabrilloResult r;
    auto bad = [&](const std::string& s) { r.problems.push_back(s); };

    const ContestDef* def = findCabrilloContest(h.contest);
    if (h.contest.empty() || h.contest.size() > 32) { bad("CONTEST must be 1 to 32 characters"); }
    for (char c : h.contest) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) { bad("CONTEST may only use A-Z, 0-9 and a hyphen"); break; }
    }
    if (!h.contest.empty() && def == nullptr) { bad("CONTEST is not one of the contests whose Cabrillo template was read; no QSO lines can be laid out"); }
    std::string myCall;
    if (!normaliseCall(h.callsign, myCall)) { bad("CALLSIGN is empty or has characters other than A-Z, 0-9 and /"); }

    if (!h.categoryAssisted.empty() && !oneOf(h.categoryAssisted, {"ASSISTED", "NON-ASSISTED"})) { bad("CATEGORY-ASSISTED value not in the specification's list"); }
    if (!h.categoryBand.empty() &&
        !oneOf(h.categoryBand, {"ALL", "160M", "80M", "40M", "20M", "15M", "10M", "6M", "4M", "2M", "222", "432", "902", "1.2G", "2.3G", "3.4G", "5.7G",
                                "10G", "24G", "47G", "75G", "122G", "134G", "241G", "Light", "VHF-3-BAND", "VHF-FM-ONLY"})) {
        bad("CATEGORY-BAND value not in the specification's list");
    }
    if (!h.categoryMode.empty() && !oneOf(h.categoryMode, {"CW", "DIGI", "FM", "RTTY", "SSB", "MIXED"})) { bad("CATEGORY-MODE value not in the specification's list"); }
    if (!h.categoryOperator.empty() && !oneOf(h.categoryOperator, {"SINGLE-OP", "MULTI-OP", "CHECKLOG"})) { bad("CATEGORY-OPERATOR value not in the specification's list"); }
    if (!h.categoryPower.empty() && !oneOf(h.categoryPower, {"HIGH", "LOW", "QRP"})) { bad("CATEGORY-POWER value not in the specification's list"); }
    if (!h.categoryStation.empty() &&
        !oneOf(h.categoryStation, {"DISTRIBUTED", "FIXED", "MOBILE", "PORTABLE", "ROVER", "ROVER-LIMITED", "ROVER-UNLIMITED", "EXPEDITION", "HQ",
                                   "SCHOOL", "EXPLORER"})) {
        bad("CATEGORY-STATION value not in the specification's list");
    }
    if (!h.categoryTime.empty() && !oneOf(h.categoryTime, {"6-HOURS", "8-HOURS", "12-HOURS", "24-HOURS"})) { bad("CATEGORY-TIME value not in the specification's list"); }
    if (!h.categoryTransmitter.empty() && !oneOf(h.categoryTransmitter, {"ONE", "TWO", "LIMITED", "UNLIMITED", "SWL"})) { bad("CATEGORY-TRANSMITTER value not in the specification's list"); }
    if (!h.categoryOverlay.empty() && !oneOf(h.categoryOverlay, {"CLASSIC", "ROOKIE", "TB-WIRES", "YOUTH", "NOVICE-TECH", "YL"})) { bad("CATEGORY-OVERLAY value not in the specification's list"); }
    if (h.claimedScore < -1) { bad("CLAIMED-SCORE must be a non-negative integer"); }
    if (h.name.size() > 75) { bad("NAME is longer than 75 characters"); }
    if (h.address.size() > 6) { bad("more than 6 ADDRESS lines"); }
    for (const std::string& a : h.address) {
        if (a.size() > 45) { bad("an ADDRESS line is longer than 45 characters"); break; }
    }
    if (!r.problems.empty()) { return r; }   // a refused header writes nothing

    std::string out;
    addTag(out, "START-OF-LOG", "3.0", eol);
    addTag(out, "CREATED-BY", cleanText(h.createdBy.empty() ? std::string("FoxSDR") : h.createdBy, 75), eol);
    addTag(out, "CONTEST", h.contest, eol);
    addTag(out, "CALLSIGN", myCall, eol);
    if (!h.location.empty()) { addTag(out, "LOCATION", cleanText(h.location, 20), eol); }
    if (!h.categoryAssisted.empty()) { addTag(out, "CATEGORY-ASSISTED", h.categoryAssisted, eol); }
    if (!h.categoryBand.empty()) { addTag(out, "CATEGORY-BAND", h.categoryBand, eol); }
    if (!h.categoryMode.empty()) { addTag(out, "CATEGORY-MODE", h.categoryMode, eol); }
    if (!h.categoryOperator.empty()) { addTag(out, "CATEGORY-OPERATOR", h.categoryOperator, eol); }
    if (!h.categoryOverlay.empty()) { addTag(out, "CATEGORY-OVERLAY", h.categoryOverlay, eol); }
    if (!h.categoryPower.empty()) { addTag(out, "CATEGORY-POWER", h.categoryPower, eol); }
    if (!h.categoryStation.empty()) { addTag(out, "CATEGORY-STATION", h.categoryStation, eol); }
    if (!h.categoryTime.empty()) { addTag(out, "CATEGORY-TIME", h.categoryTime, eol); }
    if (!h.categoryTransmitter.empty()) { addTag(out, "CATEGORY-TRANSMITTER", h.categoryTransmitter, eol); }
    if (h.claimedScore >= 0) { addTag(out, "CLAIMED-SCORE", std::to_string(h.claimedScore), eol); }
    if (!h.email.empty()) { addTag(out, "EMAIL", cleanText(h.email, 75), eol); }
    if (!h.gridLocator.empty()) { addTag(out, "GRID-LOCATOR", cleanText(h.gridLocator, 12), eol); }
    // The notes page: these five are present even when empty.
    addTag(out, "NAME", cleanText(h.name, 75), eol);
    if (h.address.empty()) { addTag(out, "ADDRESS", "", eol); }
    for (const std::string& a : h.address) { addTag(out, "ADDRESS", cleanText(a, 45), eol); }
    addTag(out, "CLUB", cleanText(h.club, 75), eol);
    // OPERATORS: space-delimited, at most 75 characters per line beginning with "OPERATORS: ".
    {
        std::string line;
        bool any = false;
        for (const std::string& o : h.operators) {
            const std::string c = cleanText(o, 20);
            if (c.empty()) { continue; }
            if (!line.empty() && 11 + line.size() + 1 + c.size() > 75) {
                addTag(out, "OPERATORS", line, eol);
                line.clear();
            }
            line += (line.empty() ? "" : " ") + c;
            any = true;
        }
        if (!line.empty() || !any) { addTag(out, "OPERATORS", line, eol); }
    }
    if (h.soapbox.empty()) { addTag(out, "SOAPBOX", "", eol); }
    for (const std::string& s : h.soapbox) { addTag(out, "SOAPBOX", cleanText(s, 75 - 9), eol); }
    for (const std::string& c : h.comments) {
        out += "X-" + cleanText(c, 70) + eol;
    }

    // QSO lines, chronological.
    std::vector<std::size_t> order(qsos.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return qsos[a].unixTime < qsos[b].unixTime; });
    for (std::size_t idx : order) {
        const Qso& q = qsos[idx];
        const std::string where = "QSO " + std::to_string(idx + 1);
        std::string call, mine;
        if (!normaliseCall(q.call, call)) { bad(where + ": the callsign worked is empty or has characters Cabrillo does not allow"); ++r.skipped; continue; }
        mine = myCall;
        if (!q.myCall.empty() && !normaliseCall(q.myCall, mine)) { bad(where + ": the logging callsign has characters Cabrillo does not allow"); ++r.skipped; continue; }
        const std::string mo = cabrilloMode(q.mode);
        if (mo.empty()) { bad(where + ": the mode has no Cabrillo code"); ++r.skipped; continue; }
        if (!(q.freqKHz > 0.0 && q.freqKHz < 1.0e9)) { bad(where + ": no usable frequency"); ++r.skipped; continue; }
        if (q.unixTime < 0) { bad(where + ": no usable time"); ++r.skipped; continue; }
        if (def->exch.size() != q.sent.size() || def->exch.size() != q.rcvd.size()) { bad(where + ": the exchange has the wrong number of fields for this contest"); ++r.skipped; continue; }
        bool tokensOk = true;
        for (std::size_t i = 0; i < def->exch.size(); ++i) {
            if (!validToken(q.sent[i], def->exch[i].kind) || !validToken(q.rcvd[i], def->exch[i].kind)) { tokensOk = false; }
        }
        if (!tokensOk) { bad(where + ": an exchange field is empty or has characters its kind does not allow"); ++r.skipped; continue; }
        const UtcParts t = utcFromUnix(q.unixTime);
        char date[16];
        char hhmm[8];
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d", t.year, t.month, t.day);
        std::snprintf(hhmm, sizeof(hhmm), "%02d%02d", t.hour, t.minute);
        char freq[16];
        std::snprintf(freq, sizeof(freq), "%.0f", q.freqKHz);
        std::string line = "QSO: " + pad(freq, 5, true) + " " + pad(mo, 2, false) + " " + date + " " + hhmm + " " + pad(mine, def->callWidth, false);
        auto side = [&](const std::vector<std::string>& toks) {
            for (std::size_t i = 0; i < def->exch.size(); ++i) {
                line += " " + pad(upperAscii(toks[i]), def->exch[i].width, false);
            }
        };
        side(q.sent);
        line += " " + pad(call, def->callWidth, false);
        side(q.rcvd);
        if (q.transmitter == 0 || q.transmitter == 1) { line += " " + std::to_string(q.transmitter); }
        while (!line.empty() && line.back() == ' ') { line.pop_back(); }
        out += line;
        out += eol;
        ++r.qsoLines;
    }
    addTag(out, "END-OF-LOG", "", eol);
    r.text = std::move(out);
    r.ok = true;
    return r;
}

// Writes the result to a file the user chose. False when the path cannot be written; never touches any other file.
inline bool writeCabrilloFile(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return (std::fclose(f) == 0) && ok;
}

}  // namespace cascade::core::ham
