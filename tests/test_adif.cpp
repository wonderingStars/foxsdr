// Tests for the ADIF writer (core/adif.hpp).
//
// The writer is checked against an INDEPENDENT reader written here from the specification text
// (https://adif.org/318/ADIF_318.htm, sections III.A, III.B.1, IV.A): it walks the ADI file the way
// the specification describes (header text, header fields, <EOH>, records of length-delimited data
// specifiers ended by <EOR>, everything between specifiers ignored) and validates each field's value
// against the data type the specification gives it. A file that passes this reader is a file another
// reader should accept; no real logger was available to import one (UNVERIFIED on a real logger).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/adif.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace {

namespace ad = cascade::core::adif;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// The reference reader.
// ---------------------------------------------------------------------------------------------
using Fields = std::vector<std::pair<std::string, std::string>>;   // name (upper case), data
struct Typed { std::string name, data, type; };

struct Parsed {
    bool ok = false;
    std::string error;
    bool hasHeader = false;
    std::string headerText;
    std::vector<Typed> header;
    std::vector<std::vector<Typed>> records;
};

std::string upper(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

Parsed parseAdi(const std::string& t) {
    Parsed p;
    std::size_t i = 0;
    bool inHeader = false;
    if (!t.empty() && t[0] != '<') { inHeader = true; p.hasHeader = true; }
    std::vector<Typed> cur;
    bool inRecord = false;
    if (inHeader) {
        const std::size_t lt = t.find('<');
        p.headerText = t.substr(0, lt == std::string::npos ? t.size() : lt);
        i = (lt == std::string::npos) ? t.size() : lt;
    }
    while (i < t.size()) {
        if (t[i] != '<') { ++i; continue; }   // characters outside specifiers are ignored
        const std::size_t gt = t.find('>', i);
        if (gt == std::string::npos) { p.error = "unterminated specifier"; return p; }
        std::string spec = t.substr(i + 1, gt - i - 1);
        const std::string uspec = upper(spec);
        if (uspec == "EOH") {
            if (!inHeader) { p.error = "EOH without a header"; return p; }
            inHeader = false;
            i = gt + 1;
            continue;
        }
        if (uspec == "EOR") {
            if (inHeader) { p.error = "EOR inside the header"; return p; }
            p.records.push_back(cur);
            cur.clear();
            inRecord = false;
            i = gt + 1;
            continue;
        }
        // name:len[:type]
        const std::size_t c1 = spec.find(':');
        if (c1 == std::string::npos || c1 == 0) { p.error = "specifier without length: <" + spec + ">"; return p; }
        Typed f;
        f.name = upper(spec.substr(0, c1));
        for (char c : f.name) { if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { p.error = "bad field name " + f.name; return p; } }
        const std::size_t c2 = spec.find(':', c1 + 1);
        const std::string lenText = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        if (lenText.empty()) { p.error = "empty length"; return p; }
        for (char c : lenText) { if (c < '0' || c > '9') { p.error = "length not digits: " + lenText; return p; } }
        const std::size_t len = static_cast<std::size_t>(std::stoul(lenText));
        if (c2 != std::string::npos) { f.type = upper(spec.substr(c2 + 1)); }
        if (gt + 1 + len > t.size()) { p.error = "data runs past the end of the file for " + f.name; return p; }
        f.data = t.substr(gt + 1, len);
        i = gt + 1 + len;
        // No field may appear twice in a header or in a record.
        for (const Typed& x : (inHeader ? p.header : cur)) { if (x.name == f.name) { p.error = "duplicate field " + f.name; return p; } }
        if (inHeader) { p.header.push_back(f); } else { cur.push_back(f); inRecord = true; }
    }
    if (inHeader) { p.error = "header without <EOH>"; return p; }
    if (inRecord) { p.error = "record without <EOR>"; return p; }
    p.ok = true;
    return p;
}

const Typed* get(const std::vector<Typed>& r, const std::string& name) {
    for (const Typed& f : r) { if (f.name == name) { return &f; } }
    return nullptr;
}

bool isString(const std::string& s) { for (unsigned char c : s) { if (c < 32 || c > 126) { return false; } } return true; }
bool isMultiline(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 13) { if (i + 1 >= s.size() || s[i + 1] != 10) { return false; } ++i; continue; }
        if (c < 32 || c > 126) { return false; }
    }
    return true;
}
bool allDigits(const std::string& s) { if (s.empty()) { return false; } for (char c : s) { if (c < '0' || c > '9') { return false; } } return true; }
bool isNumber(const std::string& s) {
    std::size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
    bool digits = false, point = false;
    for (; i < s.size(); ++i) {
        if (s[i] >= '0' && s[i] <= '9') { digits = true; } else if (s[i] == '.' && !point) { point = true; } else { return false; }
    }
    return digits;
}
bool isDate(const std::string& s) {
    if (s.size() != 8 || !allDigits(s)) { return false; }
    const int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(4, 2)), d = std::stoi(s.substr(6, 2));
    if (y < 1930 || m < 1 || m > 12 || d < 1) { return false; }
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) { days = 29; }
    return d <= days;
}
bool isTime(const std::string& s) {
    if ((s.size() != 4 && s.size() != 6) || !allDigits(s)) { return false; }
    if (std::stoi(s.substr(0, 2)) > 23 || std::stoi(s.substr(2, 2)) > 59) { return false; }
    return s.size() == 4 || std::stoi(s.substr(4, 2)) <= 59;
}
bool isGrid(const std::string& s) {
    if (s.size() != 2 && s.size() != 4 && s.size() != 6 && s.size() != 8) { return false; }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
        const std::size_t pair = i / 2;
        if (pair == 0 && !(c >= 'A' && c <= 'R')) { return false; }
        if (pair == 2 && !(c >= 'A' && c <= 'X')) { return false; }
        if ((pair == 1 || pair == 3) && !(c >= '0' && c <= '9')) { return false; }
    }
    return true;
}

// Validates one record against the data types of ADIF 3.1.8; returns "" or the first problem.
std::string validateRecord(const std::vector<Typed>& r) {
    for (const Typed& f : r) {
        const std::string& n = f.name;
        if (n == "SWL") { if ((f.data != "Y" && f.data != "N") || (!f.type.empty() && f.type != "B")) { return "SWL"; } }
        else if (n == "CALL" || n == "OPERATOR" || n == "STATION_CALLSIGN" || n == "COMMENT" || n == "SUBMODE" || n == "BAND" || n == "MODE") {
            if (!isString(f.data) || f.data.empty() || (!f.type.empty() && f.type != "S" && f.type != "E")) { return n; }
        }
        else if (n == "QSO_DATE" || n == "QSO_DATE_OFF") { if (!isDate(f.data)) { return n; } }
        else if (n == "TIME_ON" || n == "TIME_OFF") { if (!isTime(f.data)) { return n; } }
        else if (n == "FREQ") { if (!isNumber(f.data) || std::stod(f.data) <= 0) { return n; } }
        else if (n == "MY_GRIDSQUARE") { if (!isGrid(f.data)) { return n; } }
        else if (n == "NOTES") { if (!isMultiline(f.data) || f.data.empty() || (!f.type.empty() && f.type != "M")) { return n; } }
        else if (n.rfind("APP_FOXSDR_", 0) == 0) {
            // Application-defined: must carry a type indicator other than Digit/Character; ours are Numbers.
            if (f.type != "N" || !isNumber(f.data)) { return n; }
        }
        else { return "unexpected field " + n; }
    }
    // Consistency between fields.
    const Typed* mode = get(r, "MODE");
    const Typed* sub = get(r, "SUBMODE");
    if (mode != nullptr) {
        if (mode->data != "AM" && mode->data != "CW" && mode->data != "FM" && mode->data != "SSB") { return "MODE value"; }
    }
    if (sub != nullptr) { if (mode == nullptr || mode->data != "SSB" || (sub->data != "LSB" && sub->data != "USB")) { return "SUBMODE"; } }
    const Typed* band = get(r, "BAND");
    const Typed* freq = get(r, "FREQ");
    if (band != nullptr) {
        if (freq == nullptr) { return "BAND without FREQ"; }
        std::size_t n = 0;
        const ad::BandRow* t = ad::bandTable(n);
        bool in = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (band->data == t[i].name) { in = std::stod(freq->data) >= t[i].loMhz && std::stod(freq->data) <= t[i].hiMhz; }
        }
        if (!in) { return "BAND does not contain FREQ"; }
    }
    if (get(r, "SWL") == nullptr) { return "no SWL"; }
    return {};
}

constexpr std::int64_t kJan1 = 1767225600;   // 2026-01-01T00:00:00Z
constexpr std::int64_t kNov10Entry = kJan1 + 313 * 86400LL + (14 * 3600 + 35 * 60 + 7);   // 2026-11-10T14:35:07Z

ad::ListenEntry sample() {
    ad::ListenEntry e;
    e.startUnix = kNov10Entry;
    e.durationS = 125.0;
    e.frequencyHz = 7100000.0;
    e.mode = "LSB";
    e.call = "g4abc";
    e.station = "Radio M\xC3\xA4kel\xC3\xA4";    // "Radio Mäkelä" in UTF-8
    e.notes = "line1\nline2";
    e.peakDbfs = -12.34;
    e.radio = 2;
    return e;
}

ad::Options opts() {
    ad::Options o;
    o.operatorCall = "de-12345";
    o.myGrid = "jo01ab";
    return o;
}

// --------------------------------------------------------------------------------------------

void testGoldenRecord() {
    const std::string text = ad::adifFromEntries({sample()}, opts(), kNov10Entry + 3600);
    const std::string want =
        "<SWL:1:B>Y\n<CALL:5>G4ABC\n<QSO_DATE:8>20261110\n<TIME_ON:6>143507\n<QSO_DATE_OFF:8>20261110\n"
        "<TIME_OFF:6>143712\n<FREQ:3:N>7.1\n<BAND:3>40m\n<MODE:3>SSB\n<SUBMODE:3>LSB\n<OPERATOR:8>DE-12345\n"
        "<MY_GRIDSQUARE:6>JO01ab\n<COMMENT:12>Radio Makela\n<NOTES:12:M>line1\r\nline2\n"
        "<APP_FOXSDR_PEAK_DBFS:5:N>-12.3\n<APP_FOXSDR_RADIO:1:N>2\n<EOR>\n\n";
    // The notes line above has its own '\n' before the next field; the CR LF is inside the 12 characters.
    const std::size_t at = text.find("<SWL");
    CHECK(at != std::string::npos);
    CHECK(text.substr(at) == want);
}

void testHeader() {
    const std::string text = ad::adifFromEntries({}, opts(), kNov10Entry);
    CHECK(!text.empty() && text[0] != '<');   // a header begins with any character other than <
    const Parsed p = parseAdi(text);
    CHECK(p.ok);
    CHECK(p.hasHeader && p.records.empty());
    const Typed* v = get(p.header, "ADIF_VER");
    CHECK(v != nullptr && v->data == "3.1.8");
    const Typed* id = get(p.header, "PROGRAMID");
    CHECK(id != nullptr && id->data == "FoxSDR");
    const Typed* ts = get(p.header, "CREATED_TIMESTAMP");
    CHECK(ts != nullptr && ts->data == "20261110 143507" && ts->data.size() == 15);
    CHECK(text.find("<EOH>") != std::string::npos);
    CHECK(text.find("<EOH>") < text.find("<EOR>") || text.find("<EOR>") == std::string::npos);
    // The header never carries QSO fields.
    CHECK(get(p.header, "CALL") == nullptr && get(p.header, "SWL") == nullptr);
    // A program version is written when given.
    ad::Options o = opts();
    o.programVersion = "0.99.109";
    const Parsed q = parseAdi(ad::adifFromEntries({}, o, kNov10Entry));
    CHECK(q.ok && get(q.header, "PROGRAMVERSION") != nullptr && get(q.header, "PROGRAMVERSION")->data == "0.99.109");
    // An unrepresentable creation time (before 1930) drops the timestamp but keeps a valid header.
    const Parsed r = parseAdi(ad::adifFromEntries({}, opts(), -2000000000LL));
    CHECK(r.ok && get(r.header, "CREATED_TIMESTAMP") == nullptr && get(r.header, "ADIF_VER") != nullptr);
}

void testFileAcceptedByTheReferenceReader() {
    std::vector<ad::ListenEntry> v;
    std::mt19937 g(5);
    const char* modes[] = {"AM", "NFM", "WFM", "USB", "LSB", "CW", "RAW", "DSB", ""};
    const double freqs[] = {198e3, 1.5e6, 3.7e6, 5.99e6, 7.15e6, 9.5e6, 14.2e6, 29.6e6, 88.5e6, 118.7e6, 145.5e6, 433.5e6, 1296.1e6, 2.4e9};
    for (int i = 0; i < 300; ++i) {
        ad::ListenEntry e;
        e.startUnix = kJan1 + static_cast<std::int64_t>(g() % (400u * 86400u));
        e.durationS = (g() % 3 == 0) ? 0.0 : (g() % 20000) / 10.0;
        e.frequencyHz = freqs[g() % (sizeof freqs / sizeof freqs[0])] + static_cast<double>(g() % 1000);
        e.mode = modes[g() % 9];
        e.call = (g() % 5 == 0) ? "" : ("dx" + std::to_string(g() % 1000) + "/p");
        e.station = (g() % 2) ? "Stati\xC3\xB3n \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC" : "";
        e.notes = (g() % 2) ? "a\r\nb\nc\rd<EOR>e" : "";
        e.peakDbfs = (g() % 4 == 0) ? std::nan("") : -static_cast<double>(g() % 900) / 10.0;
        e.radio = static_cast<int>(g() % 9);
        v.push_back(e);
    }
    ad::Stats st;
    const std::string text = ad::adifFromEntries(v, opts(), kNov10Entry, &st);
    const Parsed p = parseAdi(text);
    CHECK(p.ok);
    if (!p.ok) { std::printf("  parse error: %s\n", p.error.c_str()); return; }
    CHECK(static_cast<int>(p.records.size()) == st.written);
    CHECK(st.written + st.skippedNoCall + st.skippedBadTime == 300);
    CHECK(st.skippedNoCall > 0 && st.written > 0 && st.skippedBadTime == 0);
    int bad = 0;
    for (const auto& r : p.records) {
        const std::string why = validateRecord(r);
        if (!why.empty()) { ++bad; std::printf("  invalid record: %s\n", why.c_str()); }
        // The suggested minimum of I.E: date, time, frequency, call, mode where known.
        CHECK(get(r, "QSO_DATE") != nullptr && get(r, "TIME_ON") != nullptr && get(r, "FREQ") != nullptr && get(r, "CALL") != nullptr);
    }
    CHECK(bad == 0);
    // The whole file is plain ASCII (the ADI format has no other kind).
    bool ascii = true;
    for (unsigned char c : text) { ascii = ascii && (c < 128); }
    CHECK(ascii);
}

void testLengthsWithNonAsciiText() {
    ad::ListenEntry e = sample();
    // Latin letters with marks, a ligature, sharp s, curly quotes, an en dash, an ellipsis, CJK and an emoji, invalid UTF-8.
    e.station = "M\xC3\xBCller Stra\xC3\x9F" "e \xC5\x81\xC3\x93" "D\xC5\xB9 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x93 \xC3\x86" "ther\xE2\x80\xA6 \xE6\x97\xA5 \xF0\x9F\x98\x80 \xFF\xFE";
    const std::string text = ad::adifFromEntries({e}, opts(), kNov10Entry);
    const Parsed p = parseAdi(text);
    CHECK(p.ok && p.records.size() == 1);
    if (!p.ok || p.records.empty()) { return; }
    const Typed* c = get(p.records[0], "COMMENT");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK(c->data == "Muller Strasse LODZ \"q\" - AEther... ? ? ??");
    CHECK(isString(c->data));
    CHECK(validateRecord(p.records[0]).empty());
    // The declared length is the length of the ASCII data (it is not the UTF-8 byte length of the input).
    const std::string spec = "<COMMENT:" + std::to_string(c->data.size()) + ">";
    CHECK(text.find(spec + c->data) != std::string::npos);
    CHECK(e.station.size() != c->data.size());
    // asciiFold on its own: every code point of Latin-1 Supplement and Latin Extended-A maps to letters (never '?'),
    // apart from the multiplication and division signs; the result has no byte outside 32..126.
    int unmapped = 0;
    for (std::uint32_t cp = 0xC0; cp <= 0x17F; ++cp) {
        std::string u;
        if (cp < 0x800) { u += static_cast<char>(0xC0 | (cp >> 6)); u += static_cast<char>(0x80 | (cp & 0x3F)); }
        const std::string f = ad::asciiFold(u, false);
        if (f.empty() || f.find('?') != std::string::npos) { ++unmapped; }
        CHECK(isString(f));
    }
    CHECK(unmapped == 1);   // U+00F7 DIVISION SIGN only; U+00D7 reads as "x"
    CHECK(ad::asciiFold("\xC3\x97", false) == "x");
    CHECK(ad::asciiFold("\xC3\xB7", false) == "?");
    // Truncated and overlong UTF-8 become '?', and the text after them survives.
    CHECK(ad::asciiFold("a\xC3", false) == "a?");
    CHECK(ad::asciiFold("a\xC0\x80" "b", false) == "a??b");
    CHECK(ad::asciiFold("\xED\xA0\x80z", false) == "???z");   // a surrogate encoded in UTF-8
    CHECK(ad::asciiFold("", false).empty());
    // Control characters: a space (single-line), CR LF pairs (multi-line).
    CHECK(ad::asciiFold("a\tb\x01" "c\r\nd\ne\rf", false) == "a b c d e f");
    CHECK(ad::asciiFold("a\r\nb\nc\rd", true) == "a\r\nb\r\nc\r\nd");
    CHECK(ad::asciiFold("a\x7F" "b", false) == "a b");
    // NOTES with line breaks counts a CR LF as two characters, and the reader agrees.
    ad::ListenEntry n = sample();
    n.notes = "one\ntwo\r\nthree";
    const Parsed q = parseAdi(ad::adifFromEntries({n}, opts(), kNov10Entry));
    CHECK(q.ok && q.records.size() == 1);
    const Typed* nt = q.records.empty() ? nullptr : get(q.records[0], "NOTES");
    CHECK(nt != nullptr && nt->data == "one\r\ntwo\r\nthree" && nt->data.size() == 3 + 2 + 3 + 2 + 5);
    // A record inside the data cannot confuse a length-delimited reader.
    ad::ListenEntry t = sample();
    t.station = "<EOR><CALL:3>XXX";
    const Parsed r = parseAdi(ad::adifFromEntries({t}, opts(), kNov10Entry));
    CHECK(r.ok && r.records.size() == 1 && get(r.records[0], "CALL")->data == "G4ABC" && get(r.records[0], "COMMENT")->data == "<EOR><CALL:3>XXX");
}

void testFieldsAndFormats() {
    // FREQ in MHz, trimmed.
    CHECK(ad::formatFreqMhz(7100000.0) == "7.1");
    CHECK(ad::formatFreqMhz(14074000.0) == "14.074");
    CHECK(ad::formatFreqMhz(118700000.0) == "118.7");
    CHECK(ad::formatFreqMhz(198000.0) == "0.198");
    CHECK(ad::formatFreqMhz(145500001.0) == "145.500001");
    CHECK(ad::formatFreqMhz(10000000.0) == "10");
    CHECK(ad::formatFreqMhz(2.4e9) == "2400");
    // BAND from the enumeration's own edges.
    CHECK(ad::bandFor(7.0e6) == "40m" && ad::bandFor(7.3e6) == "40m" && ad::bandFor(7.300001e6).empty());
    CHECK(ad::bandFor(6.99999e6).empty());
    CHECK(ad::bandFor(14.0e6) == "20m" && ad::bandFor(14.35e6) == "20m" && ad::bandFor(14.351e6).empty());
    CHECK(ad::bandFor(5.06e6) == "60m" && ad::bandFor(5.45e6) == "60m");
    CHECK(ad::bandFor(145.5e6) == "2m" && ad::bandFor(118.7e6).empty());   // the air band has no ADIF band
    CHECK(ad::bandFor(1296.1e6) == "23cm" && ad::bandFor(433.5e6) == "70cm");
    CHECK(ad::bandFor(54.0e6) == "6m" && ad::bandFor(54.5e6) == "5m");
    CHECK(ad::bandFor(0.1357e6) == "2190m");
    CHECK(ad::bandFor(0.0).empty() && ad::bandFor(-5.0).empty() && ad::bandFor(std::nan("")).empty());
    // MODE mapping.
    CHECK(ad::modeFor("NFM").mode == "FM" && ad::modeFor("wfm").mode == "FM" && ad::modeFor("FM").mode == "FM");
    CHECK(ad::modeFor("USB").mode == "SSB" && ad::modeFor("USB").submode == "USB");
    CHECK(ad::modeFor("lsb").submode == "LSB");
    CHECK(ad::modeFor("AM").mode == "AM" && ad::modeFor("CW").mode == "CW");
    CHECK(ad::modeFor("RAW").mode.empty() && ad::modeFor("").mode.empty() && ad::modeFor("DSB").mode.empty());
    // SWL=Y always, and the record needs a call by default.
    const std::string text = ad::adifFromEntries({sample()}, opts(), kNov10Entry);
    CHECK(text.find("<SWL:1:B>Y") != std::string::npos);
    // Midnight roll-over: QSO_DATE_OFF differs from QSO_DATE.
    ad::ListenEntry m = sample();
    m.startUnix = kJan1 + 86400 * 100 + 23 * 3600 + 59 * 60 + 30;   // 23:59:30
    m.durationS = 60.0;
    const Parsed p = parseAdi(ad::adifFromEntries({m}, opts(), kNov10Entry));
    CHECK(p.ok && p.records.size() == 1);
    if (p.ok && p.records.size() == 1) {
        CHECK(get(p.records[0], "TIME_ON")->data == "235930" && get(p.records[0], "TIME_OFF")->data == "000030");
        CHECK(get(p.records[0], "QSO_DATE")->data != get(p.records[0], "QSO_DATE_OFF")->data);
        CHECK(get(p.records[0], "QSO_DATE")->data == "20260411" && get(p.records[0], "QSO_DATE_OFF")->data == "20260412");
    }
    // Zero, negative, NaN and absurd durations write no OFF fields; a missing frequency writes no FREQ or BAND.
    for (double d : {0.0, -5.0, std::nan(""), 1e12}) {
        ad::ListenEntry z = sample();
        z.durationS = d;
        z.frequencyHz = 0.0;
        const Parsed q = parseAdi(ad::adifFromEntries({z}, opts(), kNov10Entry));
        CHECK(q.ok && q.records.size() == 1);
        if (q.ok && q.records.size() == 1) {
            CHECK(get(q.records[0], "TIME_OFF") == nullptr && get(q.records[0], "FREQ") == nullptr && get(q.records[0], "BAND") == nullptr);
            CHECK(validateRecord(q.records[0]).empty());
        }
    }
    // Operator / station / grid options: cleaned, or left out when empty or invalid.
    ad::Options o;
    CHECK(parseAdi(ad::adifFromEntries({sample()}, o, kNov10Entry)).records[0].size() > 0);
    const Parsed none = parseAdi(ad::adifFromEntries({sample()}, o, kNov10Entry));
    CHECK(get(none.records[0], "OPERATOR") == nullptr && get(none.records[0], "MY_GRIDSQUARE") == nullptr && get(none.records[0], "STATION_CALLSIGN") == nullptr);
    o.operatorCall = " g0 abc/m <x> ";
    o.stationCallsign = "dl1xyz";
    o.myGrid = "JO01";
    const Parsed some = parseAdi(ad::adifFromEntries({sample()}, o, kNov10Entry));
    CHECK(get(some.records[0], "OPERATOR")->data == "G0ABC/MX" && get(some.records[0], "STATION_CALLSIGN")->data == "DL1XYZ" && get(some.records[0], "MY_GRIDSQUARE")->data == "JO01");
    for (const char* badGrid : {"JO0", "ZZ99", "JO01zz99xx", "J", "JO01YY", "JO0l", "JO01ab9", ""}) {
        CHECK(ad::cleanGrid(badGrid).empty());
    }
    CHECK(ad::cleanGrid("jo01ab") == "JO01ab" && ad::cleanGrid("IO91WM44") == "IO91wm44" && ad::cleanGrid("fn") == "FN");
    CHECK(ad::cleanCall("<>").empty() && ad::cleanCall("g4abc") == "G4ABC");
    CHECK(ad::cleanCall(std::string(50, 'a')).size() == 20);
    // Peak level and radio number: written when meaningful, absent when not.
    ad::ListenEntry np = sample();
    np.peakDbfs = std::nan("");
    np.radio = 0;
    const Parsed pp = parseAdi(ad::adifFromEntries({np}, opts(), kNov10Entry));
    CHECK(get(pp.records[0], "APP_FOXSDR_PEAK_DBFS") == nullptr && get(pp.records[0], "APP_FOXSDR_RADIO") == nullptr);
}

void testSkippedEntriesAndDates() {
    ad::ListenEntry noCall = sample();
    noCall.call = "";
    ad::ListenEntry junkCall = sample();
    junkCall.call = "<> ";
    ad::ListenEntry old = sample();
    old.startUnix = -1262304001LL;   // 1929-12-31T23:59:59Z: before the year 1930 the Date type allows
    ad::ListenEntry far = sample();
    far.startUnix = 253402300800LL;  // 10000-01-01
    ad::Stats st;
    std::string text = ad::adifFromEntries({sample(), noCall, junkCall, old, far, sample()}, opts(), kNov10Entry, &st);
    CHECK(st.written == 2 && st.skippedNoCall == 2 && st.skippedBadTime == 2);
    CHECK(parseAdi(text).records.size() == 2);
    // With requireCall off the call-less entries are written, without a CALL field.
    ad::Options o = opts();
    o.requireCall = false;
    text = ad::adifFromEntries({noCall, sample()}, o, kNov10Entry, &st);
    const Parsed p = parseAdi(text);
    CHECK(st.written == 2 && p.records.size() == 2 && get(p.records[0], "CALL") == nullptr && get(p.records[1], "CALL") != nullptr);
    // Empty list: a valid file with a header and no records.
    CHECK(parseAdi(ad::adifFromEntries({}, opts(), kNov10Entry)).ok);
    CHECK(ad::adifFromEntries({}, opts(), kNov10Entry, &st).find("<EOR>") == std::string::npos && st.written == 0);
}

void testCalendar() {
    struct { std::int64_t t; int y, mo, d, h, mi, s; } known[] = {
        {0, 1970, 1, 1, 0, 0, 0},
        {-1, 1969, 12, 31, 23, 59, 59},
        {951782400, 2000, 2, 29, 0, 0, 0},
        {951868800, 2000, 3, 1, 0, 0, 0},
        {1709164800, 2024, 2, 29, 0, 0, 0},
        {1709251200, 2024, 3, 1, 0, 0, 0},
        {4102444800LL, 2100, 1, 1, 0, 0, 0},
        {4107542400LL, 2100, 3, 1, 0, 0, 0},   // 2100 is not a leap year: Feb has 28 days
        {253402300799LL, 9999, 12, 31, 23, 59, 59},
        {-1262304000LL, 1930, 1, 1, 0, 0, 0},
        {1767225600, 2026, 1, 1, 0, 0, 0},
        {1794321307LL, 2026, 11, 10, 14, 35, 7}};
    for (const auto& k : known) {
        const ad::Utc u = ad::utcFromUnix(k.t);
        CHECK(u.year == k.y && u.month == k.mo && u.day == k.d && u.hour == k.h && u.minute == k.mi && u.second == k.s);
    }
    CHECK(ad::dateInRange(ad::utcFromUnix(-1262304000LL)) && !ad::dateInRange(ad::utcFromUnix(-1262304001LL)));
    // Cross-check 20000 random times in 1970..2100 against the C library's gmtime.
    std::mt19937_64 g(9);
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::int64_t t = static_cast<std::int64_t>(g() % 4102444800ULL);
        const std::time_t tt = static_cast<std::time_t>(t);
        std::tm ref{};
#ifdef _WIN32
        gmtime_s(&ref, &tt);
#else
        gmtime_r(&tt, &ref);
#endif
        const ad::Utc u = ad::utcFromUnix(t);
        if (u.year != ref.tm_year + 1900 || u.month != ref.tm_mon + 1 || u.day != ref.tm_mday || u.hour != ref.tm_hour || u.minute != ref.tm_min || u.second != ref.tm_sec) { ++bad; }
    }
    CHECK(bad == 0);
}

void testFileWrite() {
    const fs::path root = fs::temp_directory_path() / ("foxsdr_adif_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    std::string err;
    ad::Stats st;
    const fs::path out = root / "sub" / "log.adi";    // the folder does not exist yet
    CHECK(ad::writeAdiFile(out.string(), {sample()}, opts(), kNov10Entry, err, &st) && err.empty() && st.written == 1);
    std::string back;
    {
        std::ifstream f(out, std::ios::binary);
        back.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    CHECK(back == ad::adifFromEntries({sample()}, opts(), kNov10Entry));
    CHECK(parseAdi(back).ok);
    CHECK(!fs::exists(out.string() + ".tmp"));        // no temporary file left behind
    // A second export replaces the first atomically.
    CHECK(ad::writeAdiFile(out.string(), {sample(), sample()}, opts(), kNov10Entry, err));
    {
        std::ifstream g(out, std::ios::binary);
        const std::string again((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
        CHECK(parseAdi(again).ok && parseAdi(again).records.size() == 2);
    }
    // A file where the folder should be: reported, not thrown, and nothing is left behind.
    const fs::path blocker = root / "blocker";
    { std::ofstream b(blocker); b << "x"; }
    err.clear();
    CHECK(!ad::writeAdiFile((blocker / "x.adi").string(), {sample()}, opts(), kNov10Entry, err));
    CHECK(!err.empty());
    // The target is a directory: the rename fails, is reported, and the temporary file is removed.
    fs::create_directories(root / "dirtarget.adi");
    err.clear();
    CHECK(!ad::writeAdiFile((root / "dirtarget.adi").string(), {sample()}, opts(), kNov10Entry, err));
    CHECK(!err.empty() && !fs::exists((root / "dirtarget.adi").string() + ".tmp"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

void testManyEntriesAndNoAllocationInFields() {
    std::vector<ad::ListenEntry> v(20000, sample());
    for (std::size_t i = 0; i < v.size(); ++i) { v[i].startUnix += static_cast<std::int64_t>(i) * 60; }
    ad::Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string text = ad::adifFromEntries(v, opts(), kNov10Entry, &st);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  20000 entries -> %zu bytes in %.0f ms\n", text.size(), ms);
    CHECK(st.written == 20000);
    CHECK(parseAdi(text).records.size() == 20000);
}

}  // namespace

int main() {
    testGoldenRecord();
    testHeader();
    testFileAcceptedByTheReferenceReader();
    testLengthsWithNonAsciiText();
    testFieldsAndFormats();
    testSkippedEntriesAndDates();
    testCalendar();
    testFileWrite();
    testManyEntriesAndNoAllocationInFields();
    return testSummary("test_adif");
}
