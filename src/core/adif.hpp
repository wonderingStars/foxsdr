// ADIF 3.1.8 writer for the listening log (SWL reports): the ADI file format, the field names and
// types the specification defines, the escaping and length rules, and the export of a list of
// entries. Nothing here is wired into the engine (docs/feature-records/signal-blocks.md, "Wiring
// hand-off", says where it goes). Design: pre-release-features-design.md section 14.
//
// SOURCE. The ADIF 3.1.8 specification text, https://adif.org/318/ADIF_318.htm, READ IN FULL for this
// block (sections III.A data types, III.B.1 Band and Mode enumerations, III.C header and QSO fields,
// IV.A the ADI file format). What the code relies on, and where it is stated there:
//   * IV.A.1: a data specifier is <FIELD:LENGTH[:TYPE]>DATA, the length an unsigned decimal count of the data
//     (characters; the ADI format admits ASCII only, so characters and bytes are the same number); field
//     names are case-insensitive; "Fields of type IntlCharacter, IntlString, and IntlMultilineString cannot be
//     used in ADI files". So COMMENT_INTL / NOTES_INTL / NAME_INTL are NOT written, and text with characters
//     outside printable ASCII is FOLDED TO ASCII here (see asciiFold) - lossily, which is the price of the ADI
//     format; the CSV export is the lossless one.
//   * III.A: String = a sequence of Characters (ASCII 32..126); MultilineString = Characters and line breaks, a
//     line break being CR LF; Date = YYYYMMDD (UTC, year >= 1930); Time = HHMMSS or HHMM (UTC); Number = digits
//     with an optional sign and one decimal point; Boolean = Y or N.
//   * IV.A.2-3: a file is an optional header then records; a header "begins with any character other than <" and
//     ends with <EOH>; it may carry arbitrary text and the header fields ADIF_VER, CREATED_TIMESTAMP (15
//     characters "YYYYMMDD HHMMSS"), PROGRAMID, PROGRAMVERSION. IV.A.6: a record is data specifiers then <EOR>;
//     characters between specifiers are ignored.
//   * III.C.1.b: SWL (Boolean) "indicates that the QSO information pertains to an SWL report"; CALL "the contacted
//     station's callsign"; FREQ "QSO frequency in Megahertz" (Number); BAND (enumeration, with lower/upper
//     frequency in MHz per band); MODE (enumeration: AM, CW, FM, SSB ... with submodes LSB and USB under SSB);
//     QSO_DATE / TIME_ON / QSO_DATE_OFF / TIME_OFF; OPERATOR "the logging operator's callsign; if STATION_CALLSIGN
//     is absent, OPERATOR shall be treated as both the logging station's callsign and the logging operator's";
//     MY_GRIDSQUARE (2, 4, 6 or 8 characters); COMMENT (String), NOTES (MultilineString).
//   * IV.A.4: application-defined fields are APP_<PROGRAMID>_<FIELDNAME> with a type indicator (used for
//     APP_FOXSDR_PEAK_DBFS and APP_FOXSDR_RADIO).
//   * I.E suggests QSO_DATE, TIME_ON, FREQ and/or BAND, CALL and MODE as what a record should carry; the
//     specification mandates no field.
// UNVERIFIED: which of OPERATOR and STATION_CALLSIGN a short-wave LISTENER's identifier belongs in. The
// specification defines both for a logging station; an SWL report has no over-the-air callsign, so OPERATOR
// is written (STATION_CALLSIGN only if the caller supplies one). No logger was available to import the file.
//
// THREADS. Pure functions; adifFromEntries/writeAdiFile allocate and touch the disk only in writeAdiFile: a
// WORKER thread call, never the pass or the DSP thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace cascade::core::adif {

// One thing heard. Times are UTC seconds since 1970-01-01.
struct ListenEntry {
    std::int64_t startUnix = 0;
    double durationS = 0.0;
    double frequencyHz = 0.0;
    std::string mode;     // FoxSDR mode name: "AM", "NFM", "WFM", "FM", "USB", "LSB", "CW" ...
    std::string call;     // the station heard (typed, or from a bookmark); may be empty
    std::string station;  // a name for it ("BBC World Service", "Tower"); goes into COMMENT
    std::string notes;    // free text; goes into NOTES (line breaks kept as CR LF)
    double peakDbfs = std::nan("");
    int radio = 0;        // 1..8, 0 = unknown
};

struct Options {
    std::string operatorCall;     // the listener's identifier: written as OPERATOR when it has any valid characters
    std::string stationCallsign;  // written as STATION_CALLSIGN when given (normally empty for a listener)
    std::string myGrid;           // Maidenhead 2/4/6/8 characters; written as MY_GRIDSQUARE when valid
    std::string programVersion = "";
    bool requireCall = true;      // an SWL report without a CALL is not a report: skip such entries
};

struct Stats {
    int written = 0;
    int skippedNoCall = 0;
    int skippedBadTime = 0;
};

// --- text ----------------------------------------------------------------------------

// Folds UTF-8 text to printable ASCII. Latin letters with marks lose them (U+00C0..U+017F), a few ligatures
// and typographic marks get ASCII spellings (Æ -> AE, ß -> ss, curly quotes, dashes, ellipsis, NBSP), every
// other code point and every invalid UTF-8 byte becomes '?'. Control characters become a space (TAB, CR, LF
// included), or - when `keepLineBreaks` - CR LF / LF / CR each become one CR LF pair. The result contains only
// bytes 32..126 (and CR LF when keepLineBreaks), so its byte length IS its character length.
inline std::string asciiFold(const std::string& utf8, bool keepLineBreaks) {
    // U+00C0..U+00FF
    static const char* const kLatin1[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "?", "o", "u", "u", "u", "u", "y", "th", "y"};
    // U+0100..U+017F, space separated (128 entries; checked by the test)
    static const char kExtA[] =
        "A a A a A a C c C c C c C c D d D d E e E e E e E e E e G g G g G g G g H h H h "
        "I i I i I i I i I i IJ ij J j K k k L l L l L l L l L l N n N n N n n N n "
        "O o O o O o OE oe R r R r R r S s S s S s S s T t T t T t "
        "U u U u U u U u U u U u W w Y y Y Z z Z z Z z s";
    static const std::vector<std::string> extA = [] {
        std::vector<std::string> v;
        const char* p = kExtA;
        while (*p != '\0') {
            const char* q = p;
            while (*q != '\0' && *q != ' ') { ++q; }
            v.emplace_back(p, q);
            p = (*q == ' ') ? q + 1 : q;
        }
        return v;
    }();

    std::string out;
    out.reserve(utf8.size());
    auto push = [&](const char* s) { out += s; };
    std::size_t i = 0;
    const std::size_t n = utf8.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        bool valid = true;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        } else {
            valid = false;
        }
        if (valid && len > 1) {
            if (i + len > n) {
                valid = false;
            } else {
                cp = c & (0xFFu >> (len + 1));
                for (std::size_t k = 1; k < len; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
                    if ((cc & 0xC0) != 0x80) { valid = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (valid && ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) {
                    valid = false;  // overlong or surrogate
                }
            }
        }
        if (!valid) {
            out += '?';
            ++i;  // resynchronise one byte on
            continue;
        }
        i += len;
        if (cp == '\r' || cp == '\n') {
            if (keepLineBreaks) {
                if (cp == '\r' && i < n && utf8[i] == '\n') { ++i; }
                push("\r\n");
            } else {
                if (cp == 13 && i < n && utf8[i] == 10) { ++i; }  // a CR LF pair is one space
                out += ' ';
            }
        } else if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0)) {
            out += ' ';
        } else if (cp < 127) {
            out += static_cast<char>(cp);
        } else if (cp == 0xA0) {
            out += ' ';
        } else if (cp >= 0xC0 && cp <= 0xFF) {
            push(kLatin1[cp - 0xC0]);
        } else if (cp >= 0x100 && cp <= 0x17F && extA.size() == 128) {
            push(extA[cp - 0x100].c_str());
        } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A) {
            out += '\'';
        } else if (cp == 0x201C || cp == 0x201D || cp == 0x201E) {
            out += '"';
        } else if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2013 || cp == 0x2014 || cp == 0x2212) {
            out += '-';
        } else if (cp == 0x2026) {
            push("...");
        } else {
            out += '?';
        }
    }
    return out;
}

// A callsign / listener identifier: upper case, only A-Z 0-9 '/' '-' kept (after folding), at most 20 characters.
inline std::string cleanCall(const std::string& s) {
    const std::string f = asciiFold(s, false);
    std::string out;
    for (char ch : f) {
        const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/' || u == '-') { out += u; }
        if (out.size() >= 20) { break; }
    }
    return out;
}

// A Maidenhead locator of 2, 4, 6 or 8 characters (AA, AA00, AA00aa, AA00aa00); returns it normalised (field letters
// upper case, subsquare letters lower case, as the locator convention writes them) or "" when it is not one.
inline std::string cleanGrid(const std::string& s) {
    if (s.size() != 2 && s.size() != 4 && s.size() != 6 && s.size() != 8) { return {}; }
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const std::size_t pair = i / 2;
        const bool letterPair = (pair == 0 || pair == 2);
        if (letterPair) {
            const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (pair == 0 && !(u >= 'A' && u <= 'R')) { return {}; }
            if (pair == 2 && !(u >= 'A' && u <= 'X')) { return {}; }
            out[i] = (pair == 0) ? u : static_cast<char>(std::tolower(static_cast<unsigned char>(u)));
        } else if (!(c >= '0' && c <= '9')) {
            return {};
        }
    }
    return out;
}

// --- numbers and dates --------------------------------------------------------------------

struct Utc {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// UTC calendar fields for a Unix time. Days-to-civil by the standard proleptic Gregorian algorithm (H. Hinnant,
// "chrono-Compatible Low-Level Date Algorithms"); exact for every year, no time-zone and no gmtime.
inline Utc utcFromUnix(std::int64_t t) {
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    Utc u;
    u.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    u.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    u.year = static_cast<int>(y + (u.month <= 2 ? 1 : 0));
    u.hour = static_cast<int>(rem / 3600);
    u.minute = static_cast<int>((rem % 3600) / 60);
    u.second = static_cast<int>(rem % 60);
    return u;
}

inline bool dateInRange(const Utc& u) { return u.year >= 1930 && u.year <= 9999; }

// FREQ in MHz as an ADIF Number: up to six decimals (1 Hz), trailing zeros and a bare point removed.
inline std::string formatFreqMhz(double hz) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", hz / 1.0e6);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') { s.pop_back(); }
    if (!s.empty() && s.back() == '.') { s.pop_back(); }
    return s;
}

// The ADIF Band enumeration (3.1.8 section III.B.1, lower/upper frequency in MHz). A frequency outside every
// band (a broadcast band between two amateur bands, the air band) has none: the BAND field is then omitted.
struct BandRow { const char* name; double loMhz, hiMhz; };
inline const BandRow* bandTable(std::size_t& count) {
    static const BandRow kBands[] = {
        {"2190m", 0.1357, 0.1378}, {"630m", 0.472, 0.479},   {"560m", 0.501, 0.504},  {"160m", 1.8, 2.0},
        {"80m", 3.5, 4.0},         {"60m", 5.06, 5.45},      {"40m", 7.0, 7.3},       {"30m", 10.1, 10.15},
        {"20m", 14.0, 14.35},      {"17m", 18.068, 18.168},  {"15m", 21.0, 21.45},    {"12m", 24.89, 24.99},
        {"10m", 28.0, 29.7},       {"8m", 40.0, 45.0},       {"6m", 50.0, 54.0},      {"5m", 54.000001, 69.9},
        {"4m", 70.0, 71.0},        {"2m", 144.0, 148.0},     {"1.25m", 222.0, 225.0}, {"70cm", 420.0, 450.0},
        {"33cm", 902.0, 928.0},    {"23cm", 1240.0, 1300.0}, {"13cm", 2300.0, 2450.0}, {"9cm", 3300.0, 3500.0},
        {"6cm", 5650.0, 5925.0},   {"3cm", 10000.0, 10500.0}, {"1.25cm", 24000.0, 24250.0}, {"6mm", 47000.0, 47200.0},
        {"4mm", 75500.0, 81000.0}, {"2.5mm", 119980.0, 123000.0}, {"2mm", 134000.0, 149000.0}, {"1mm", 241000.0, 250000.0},
        {"submm", 300000.0, 7500000.0}};
    count = sizeof kBands / sizeof kBands[0];
    return kBands;
}

// The band name for a frequency (inclusive of both edges, as the table gives them), or "".
inline std::string bandFor(double frequencyHz) {
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) { return {}; }
    const double mhz = frequencyHz / 1.0e6;
    std::size_t n = 0;
    const BandRow* t = bandTable(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (mhz >= t[i].loMhz && mhz <= t[i].hiMhz) { return t[i].name; }
    }
    return {};
}

// FoxSDR mode name -> ADIF MODE (+ SUBMODE). Both empty for a mode the ADIF enumeration has no entry for.
struct ModeMap { std::string mode, submode; };
inline ModeMap modeFor(const std::string& foxMode) {
    std::string m;
    for (char c : foxMode) { m += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    if (m == "AM") { return {"AM", ""}; }
    if (m == "NFM" || m == "WFM" || m == "FM") { return {"FM", ""}; }
    if (m == "USB") { return {"SSB", "USB"}; }
    if (m == "LSB") { return {"SSB", "LSB"}; }
    if (m == "CW") { return {"CW", ""}; }
    return {};
}

// --- the writer ----------------------------------------------------------------------------

// One data specifier: <NAME:LEN[:T]>DATA, LEN = the byte (= character) count of DATA. `value` must already be ASCII.
inline std::string field(const char* name, const std::string& value, char type = 0) {
    std::string s = "<";
    s += name;
    s += ':';
    s += std::to_string(value.size());
    if (type != 0) {
        s += ':';
        s += type;
    }
    s += '>';
    s += value;
    return s;
}

inline std::string two(int v) { char b[8]; std::snprintf(b, sizeof b, "%02d", v); return b; }
inline std::string four(int v) { char b[8]; std::snprintf(b, sizeof b, "%04d", v); return b; }
inline std::string dateOf(const Utc& u) { return four(u.year) + two(u.month) + two(u.day); }
inline std::string timeOf(const Utc& u) { return two(u.hour) + two(u.minute) + two(u.second); }

// The header: a line of text (which must not begin with '<'), the header fields, <EOH>.
inline std::string header(const Options& o, std::int64_t createdUnix) {
    const Utc c = utcFromUnix(createdUnix);
    std::string h = "FoxSDR listening log - ADIF 3.1.8 (SWL reports)\n";
    h += field("ADIF_VER", "3.1.8") + "\n";
    h += field("PROGRAMID", "FoxSDR") + "\n";
    if (!o.programVersion.empty()) { h += field("PROGRAMVERSION", asciiFold(o.programVersion, false)) + "\n"; }
    if (dateInRange(c)) { h += field("CREATED_TIMESTAMP", dateOf(c) + " " + timeOf(c)) + "\n"; }
    h += "<EOH>\n\n";
    return h;
}

// One record. Empty string (and the reason in `why`: 1 no call, 2 bad time) when the entry is not exportable.
inline std::string record(const ListenEntry& e, const Options& o, int* why = nullptr) {
    if (why != nullptr) { *why = 0; }
    const std::string call = cleanCall(e.call);
    if (o.requireCall && call.empty()) {
        if (why != nullptr) { *why = 1; }
        return {};
    }
    const Utc on = utcFromUnix(e.startUnix);
    if (!dateInRange(on)) {
        if (why != nullptr) { *why = 2; }
        return {};
    }
    std::string r;
    auto add = [&](const std::string& f) { r += f; r += '\n'; };
    add(field("SWL", "Y", 'B'));
    if (!call.empty()) { add(field("CALL", call)); }
    add(field("QSO_DATE", dateOf(on)));
    add(field("TIME_ON", timeOf(on)));
    if (std::isfinite(e.durationS) && e.durationS > 0.0 && e.durationS < 86400.0 * 366.0) {
        const Utc off = utcFromUnix(e.startUnix + static_cast<std::int64_t>(std::llround(e.durationS)));
        if (dateInRange(off)) {
            add(field("QSO_DATE_OFF", dateOf(off)));
            add(field("TIME_OFF", timeOf(off)));
        }
    }
    if (std::isfinite(e.frequencyHz) && e.frequencyHz > 0.0) {
        add(field("FREQ", formatFreqMhz(e.frequencyHz), 'N'));
        const std::string band = bandFor(e.frequencyHz);
        if (!band.empty()) { add(field("BAND", band)); }
    }
    const ModeMap mm = modeFor(e.mode);
    if (!mm.mode.empty()) {
        add(field("MODE", mm.mode));
        if (!mm.submode.empty()) { add(field("SUBMODE", mm.submode)); }
    }
    const std::string op = cleanCall(o.operatorCall);
    if (!op.empty()) { add(field("OPERATOR", op)); }
    const std::string sc = cleanCall(o.stationCallsign);
    if (!sc.empty()) { add(field("STATION_CALLSIGN", sc)); }
    const std::string grid = cleanGrid(o.myGrid);
    if (!grid.empty()) { add(field("MY_GRIDSQUARE", grid)); }
    const std::string station = asciiFold(e.station, false);
    if (!station.empty()) { add(field("COMMENT", station)); }
    const std::string notes = asciiFold(e.notes, true);
    if (!notes.empty()) { add(field("NOTES", notes, 'M')); }
    if (std::isfinite(e.peakDbfs)) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", e.peakDbfs);
        add(field("APP_FOXSDR_PEAK_DBFS", b, 'N'));
    }
    if (e.radio >= 1 && e.radio <= 99) { add(field("APP_FOXSDR_RADIO", std::to_string(e.radio), 'N')); }
    r += "<EOR>\n\n";
    return r;
}

// The whole file as text: the header, then one record per exportable entry in the order given. `stats` (optional)
// says what was written and why entries were left out.
inline std::string adifFromEntries(const std::vector<ListenEntry>& entries, const Options& o, std::int64_t createdUnix,
                                   Stats* stats = nullptr) {
    Stats local;
    Stats& st = (stats != nullptr) ? *stats : local;
    st = Stats{};
    std::string out = header(o, createdUnix);
    for (const ListenEntry& e : entries) {
        int why = 0;
        const std::string r = record(e, o, &why);
        if (r.empty()) {
            if (why == 1) { ++st.skippedNoCall; }
            if (why == 2) { ++st.skippedBadTime; }
            continue;
        }
        out += r;
        ++st.written;
    }
    return out;
}

// WORKER THREAD. Writes the file at `path` (created or replaced) through a temporary file next to it and a rename, so
// an interrupted export never leaves a half-written .adi under the final name. False with the reason in `error` when
// the folder cannot be made, the file cannot be written, or the rename fails; the temporary file is removed then.
inline bool writeAdiFile(const std::string& path, const std::vector<ListenEntry>& entries, const Options& o,
                         std::int64_t createdUnix, std::string& error, Stats* stats = nullptr) {
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) { error = "cannot create the folder: " + ec.message(); return false; }
    }
    const std::string text = adifFromEntries(entries, o, createdUnix, stats);
    const std::filesystem::path tmp = p.string() + ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) { error = "cannot create " + tmp.string(); return false; }
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::filesystem::remove(tmp, ec);
        error = "cannot write " + tmp.string() + " (disk full or removed)";
        return false;
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        error = "cannot replace " + p.string();
        return false;
    }
    return true;
}

}  // namespace cascade::core::adif
