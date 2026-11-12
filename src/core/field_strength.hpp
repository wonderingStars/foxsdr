// field_strength.hpp - ANTENNA FACTOR, CABLE LOSS and PATH LOSS tables (user-supplied CSV, interpolated) and the dBuV/m field-strength readout,
// with free-space path loss and the Friis relation.
//
// STAND-ALONE (lane rfmon): nothing in the engine uses it. SHIPS NO MANUFACTURER DATA: every table comes from a file the user supplies.
//
// TABLE FILE (CSV, one point per line): `<frequency>,<value dB>`; the separator may be a comma, semicolon, tab or spaces; `#` starts a comment;
// ONE header line is tolerated, and only as the first significant line, when its first character is a letter. The frequency unit is NOT in the
// file: the caller says Hz, kHz, MHz or GHz when loading (a file read in the wrong unit is the user's error to see in the plot; a wrong guess
// by the program would be a silent one). Frequencies must be strictly increasing (anything else is refused with its line, never sorted: a
// table that is out of order is more likely a broken export than a table to repair). Frequency in (0, 1e13] Hz, value in [-400, 400] dB,
// finite numbers in decimal notation only, <= 65536 points, file <= 1 MiB, line <= 200 bytes.
//
// INTERPOLATION: linear in the value against log10(frequency) between neighbouring points (the usual treatment of antenna-factor and
// cable-loss curves). Outside the first and last point the end value is held and the lookup SAYS SO (LookupStatus::Below / Above): a field
// strength computed from a held value is flagged, never presented as measured. A one-point table is a constant, in range only at its own
// frequency.
//
// FORMULAS (E in V/m, V in volts across R, f in Hz, d in metres, c = 299 792 458 m/s, eta = 376.730313668 ohm):
//   antenna factor           AF = E / V                    in dB(1/m): AF = E[dBV/m] - V[dBV]            (en.wikipedia.org/wiki/Antenna_factor, CC BY-SA 4.0,
//                                                                       opened 2026-11-11, which also derives AF = 9.73 / (lambda sqrt(G)) for 50 ohm)
//   AF from gain             AF = sqrt(4 pi eta / (R G lambda^2))  -> 9.73 / (lambda sqrt G) at R = 50, G linear
//   volts from power         V[dBuV] = P[dBm] + 90 + 10 log10 R      (V = sqrt(P R); 106.99 dBuV per 0 dBm at 50 ohm)
//   field strength           E[dBuV/m] = V[dBuV] + AF[dB/m] + cable loss[dB] - preamp gain[dB]
//   free-space path loss     FSPL = (4 pi d f / c)^2, 20 log10(4 pi d f / c) dB = 20 log10 d + 20 log10 f - 147.55   (en.wikipedia.org/wiki/Free-space_path_loss,
//                                                                       CC BY-SA 4.0, opened 2026-11-11, which also gives 92.45 for d in km and f in GHz)
//   Friis                    Pr = Pt + Gt + Gr - FSPL (dB)    (the same page: Pr/Pt = Dt Dr (lambda / 4 pi d)^2)
//   field of a radiator      E = sqrt(eta EIRP / (4 pi d^2))  -> 104.77 dBuV/m at 1 m for 1 mW EIRP (power density S = EIRP / 4 pi d^2, E = sqrt(eta S): the
//                                                                       textbook far-field relation, derived here rather than quoted from a page; the test
//                                                                       closes the loop: EIRP -> Friis -> volts -> + AF -> the same field)
// Heads-up: the test cross-checks all of these against each other, not against an instrument.
//
// THREADS: tables are plain values; one thread owns one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace cascade::core {

inline constexpr std::size_t kLossMaxBytes = 1024 * 1024;
inline constexpr std::size_t kLossMaxLine = 200;
inline constexpr std::size_t kLossMaxPoints = 65536;
inline constexpr double kLossMaxFreqHz = 1e13;
inline constexpr double kLossMaxValueDb = 400.0;

inline constexpr double kSpeedOfLight = 299792458.0;
inline constexpr double kFreeSpaceImpedance = 376.730313668;
inline constexpr double kPi = 3.14159265358979323846264338327950288;

enum class FreqUnit { Hz, kHz, MHz, GHz };
inline double freqUnitHz(FreqUnit u) {
    switch (u) {
        case FreqUnit::Hz: return 1.0;
        case FreqUnit::kHz: return 1e3;
        case FreqUnit::MHz: return 1e6;
        case FreqUnit::GHz: return 1e9;
    }
    return 1.0;
}

enum class LossError {
    None,
    TooLarge,
    LineTooLong,
    BadNumber,        // missing, malformed, non-finite or out of range; also a second header line
    NotIncreasing,    // frequency not strictly above the previous point's
    TooManyPoints,
    Empty,
    Unreadable,
};

struct LossParseResult {
    LossError error = LossError::None;
    std::size_t line = 0;
    bool ok() const { return error == LossError::None; }
};

inline const char* lossErrorText(LossError e) {
    switch (e) {
        case LossError::None: return "ok";
        case LossError::TooLarge: return "table file too large";
        case LossError::LineTooLong: return "table line too long";
        case LossError::BadNumber: return "bad number in table";
        case LossError::NotIncreasing: return "table frequencies must increase";
        case LossError::TooManyPoints: return "too many points in table";
        case LossError::Empty: return "table has no points";
        case LossError::Unreadable: return "table file could not be read";
    }
    return "?";
}

enum class LookupStatus { InRange, Below, Above, Empty };

struct LookupResult {
    double valueDb = 0.0;
    LookupStatus status = LookupStatus::Empty;
    bool inRange() const { return status == LookupStatus::InRange; }
};

class LossTable {
public:
    struct Point {
        double fHz;
        double db;
    };

    bool empty() const { return pts_.empty(); }
    std::size_t size() const { return pts_.size(); }
    const std::vector<Point>& points() const { return pts_; }
    double minHz() const { return pts_.empty() ? 0.0 : pts_.front().fHz; }
    double maxHz() const { return pts_.empty() ? 0.0 : pts_.back().fHz; }
    void clear() { pts_.clear(); }

    // Builds a table from points (for code that has them already); same refusals as the file.
    LossParseResult setPoints(const std::vector<Point>& p) {
        LossParseResult r;
        if (p.empty()) { r.error = LossError::Empty; return r; }
        if (p.size() > kLossMaxPoints) { r.error = LossError::TooManyPoints; return r; }
        double prev = 0.0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (!std::isfinite(p[i].fHz) || !(p[i].fHz > 0.0) || p[i].fHz > kLossMaxFreqHz || !std::isfinite(p[i].db) || std::fabs(p[i].db) > kLossMaxValueDb) {
                r.error = LossError::BadNumber;
                r.line = i + 1;
                return r;
            }
            if (i > 0 && !(p[i].fHz > prev)) { r.error = LossError::NotIncreasing; r.line = i + 1; return r; }
            prev = p[i].fHz;
        }
        pts_ = p;
        return r;
    }

    // Parses the CSV text with the frequency column in `unit`. On failure the table is left EMPTY.
    LossParseResult parse(const char* text, std::size_t len, FreqUnit unit) {
        pts_.clear();
        LossParseResult r;
        if (text == nullptr && len != 0) { r.error = LossError::BadNumber; return r; }
        if (len > kLossMaxBytes) { r.error = LossError::TooLarge; return r; }
        const double mul = freqUnitHz(unit);
        std::vector<Point> p;
        bool seenSignificant = false;
        std::size_t pos = 0, lineNo = 0;
        while (pos < len) {
            std::size_t eol = pos;
            while (eol < len && text[eol] != '\n') { ++eol; }
            ++lineNo;
            std::size_t a = pos, b = eol;
            pos = eol + 1;
            if (b > a && text[b - 1] == '\r') { --b; }
            if (b - a > kLossMaxLine) { r.error = LossError::LineTooLong; r.line = lineNo; return r; }
            const char* q = text + a;
            const char* end = text + b;
            skipBlank(q, end);
            if (q >= end || *q == '#') { continue; }
            const bool first = !seenSignificant;
            seenSignificant = true;
            if (first && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z'))) { continue; }   // the one tolerated header line
            double f = 0, v = 0;
            if (!number(q, end, f) || !separator(q, end) || !number(q, end, v)) { r.error = LossError::BadNumber; r.line = lineNo; return r; }
            skipBlank(q, end);
            if (q < end && *q != '#') { r.error = LossError::BadNumber; r.line = lineNo; return r; }
            const double fHz = f * mul;
            if (!std::isfinite(fHz) || !(fHz > 0.0) || fHz > kLossMaxFreqHz || std::fabs(v) > kLossMaxValueDb) { r.error = LossError::BadNumber; r.line = lineNo; return r; }
            if (!p.empty() && !(fHz > p.back().fHz)) { r.error = LossError::NotIncreasing; r.line = lineNo; return r; }
            if (p.size() >= kLossMaxPoints) { r.error = LossError::TooManyPoints; r.line = lineNo; return r; }
            p.push_back({fHz, v});
        }
        if (p.empty()) { r.error = LossError::Empty; return r; }
        pts_ = std::move(p);
        return r;
    }
    LossParseResult parse(const std::string& s, FreqUnit unit) { return parse(s.data(), s.size(), unit); }

    LossParseResult loadFile(const std::string& path, FreqUnit unit) {
        pts_.clear();
        std::ifstream f(path, std::ios::binary);
        if (!f) { LossParseResult r; r.error = LossError::Unreadable; return r; }
        std::string buf(kLossMaxBytes + 1, '\0');
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        buf.resize(static_cast<std::size_t>(f.gcount()));
        return parse(buf, unit);
    }

    // The value at fHz, linear in dB against log10(f); the end value is held outside the table and the status says so.
    LookupResult lookup(double fHz) const {
        LookupResult r;
        if (pts_.empty() || !(fHz > 0.0) || !std::isfinite(fHz)) { return r; }
        if (pts_.size() == 1) {
            r.valueDb = pts_[0].db;
            r.status = fHz == pts_[0].fHz ? LookupStatus::InRange : (fHz < pts_[0].fHz ? LookupStatus::Below : LookupStatus::Above);
            return r;
        }
        if (fHz < pts_.front().fHz) { r.valueDb = pts_.front().db; r.status = LookupStatus::Below; return r; }
        if (fHz > pts_.back().fHz) { r.valueDb = pts_.back().db; r.status = LookupStatus::Above; return r; }
        std::size_t hi = static_cast<std::size_t>(std::lower_bound(pts_.begin(), pts_.end(), fHz, [](const Point& p, double f) { return p.fHz < f; }) - pts_.begin());
        if (hi == 0) { hi = 1; }
        const Point& a = pts_[hi - 1];
        const Point& b = pts_[hi];
        const double t = (std::log10(fHz) - std::log10(a.fHz)) / (std::log10(b.fHz) - std::log10(a.fHz));
        r.valueDb = a.db + std::min(1.0, std::max(0.0, t)) * (b.db - a.db);
        r.status = LookupStatus::InRange;
        return r;
    }

private:
    static void skipBlank(const char*& p, const char* end) {
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
    }
    // One field up to a separator, decimal notation only (strtod would take hex floats, inf and nan).
    static bool number(const char*& p, const char* end, double& out) {
        skipBlank(p, end);
        const char* s = p;
        while (p < end && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' && *p != '#') { ++p; }
        const std::size_t len = static_cast<std::size_t>(p - s);
        char buf[48];
        if (len == 0 || len >= sizeof(buf)) { return false; }
        for (std::size_t i = 0; i < len; ++i) {
            const char ch = s[i];
            if (!((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.' || ch == 'e' || ch == 'E')) { return false; }
        }
        std::memcpy(buf, s, len);
        buf[len] = '\0';
        char* e = nullptr;
        const double v = std::strtod(buf, &e);
        if (e == buf || *e != '\0' || !std::isfinite(v)) { return false; }
        out = v;
        return true;
    }
    // A comma or semicolon (with blanks around it), or blanks alone.
    static bool separator(const char*& p, const char* end) {
        const char* s = p;
        skipBlank(p, end);
        if (p < end && (*p == ',' || *p == ';')) { ++p; return true; }
        return p > s && p < end;
    }
    std::vector<Point> pts_;
};

// ----- Formulas -------------------------------------------------------------------------------------------------------------------------

inline double wavelengthM(double fHz) { return kSpeedOfLight / fHz; }

// dBm into R ohm -> dBuV.
inline double dbmToDbuv(double dbm, double rOhm = 50.0) { return dbm + 90.0 + 10.0 * std::log10(rOhm); }
inline double dbuvToDbm(double dbuv, double rOhm = 50.0) { return dbuv - 90.0 - 10.0 * std::log10(rOhm); }

// Antenna factor in dB(1/m) of an antenna of gain `gainDbi` at fHz into R ohm: sqrt(4 pi eta / (R G lambda^2)).
inline double antennaFactorFromGainDb(double fHz, double gainDbi, double rOhm = 50.0) {
    const double lam = wavelengthM(fHz);
    return 10.0 * std::log10(4.0 * kPi * kFreeSpaceImpedance / (rOhm * lam * lam)) - gainDbi;
}
inline double gainDbiFromAntennaFactor(double fHz, double afDb, double rOhm = 50.0) {
    const double lam = wavelengthM(fHz);
    return 10.0 * std::log10(4.0 * kPi * kFreeSpaceImpedance / (rOhm * lam * lam)) - afDb;
}

// Free-space path loss in dB (positive) over d metres at fHz: 20 log10(4 pi d f / c). Zero when an argument is unusable.
inline double fsplDb(double dM, double fHz) {
    if (!(dM > 0.0) || !(fHz > 0.0) || !std::isfinite(dM) || !std::isfinite(fHz)) { return 0.0; }
    return 20.0 * std::log10(4.0 * kPi * dM * fHz / kSpeedOfLight);
}

// Friis: received dBm = Pt + Gt + Gr - FSPL.
inline double friisReceivedDbm(double ptDbm, double gtDbi, double grDbi, double dM, double fHz) { return ptDbm + gtDbi + grDbi - fsplDb(dM, fHz); }

// Far-field strength in dBuV/m at d metres from a radiator of the given EIRP: E = sqrt(eta EIRP / (4 pi d^2)).
inline double fieldFromEirpDbuvPerM(double eirpDbm, double dM) {
    if (!(dM > 0.0) || !std::isfinite(dM)) { return 0.0; }
    const double eirpW = std::pow(10.0, (eirpDbm - 30.0) / 10.0);
    return 10.0 * std::log10(kFreeSpaceImpedance * eirpW / (4.0 * kPi * dM * dM)) + 120.0;
}

// The measured field strength in dBuV/m from a level at the analyser input, with the corrections.
struct FieldStrengthInputs {
    double levelDbm = 0.0;           // at the analyser input
    double rOhm = 50.0;
    double afDb = 0.0;               // antenna factor, dB(1/m)
    double cableLossDb = 0.0;        // positive loss between antenna and analyser
    double preampGainDb = 0.0;       // positive gain ahead of the analyser input (subtracted)
};
inline double fieldStrengthDbuvPerM(const FieldStrengthInputs& in) {
    return dbmToDbuv(in.levelDbm, in.rOhm) + in.afDb + in.cableLossDb - in.preampGainDb;
}

struct FieldReadout {
    double dbuvPerM = 0.0;
    double afDb = 0.0;
    double cableLossDb = 0.0;
    bool afHeld = false;             // the antenna factor was held from the table's end (outside its range)
    bool cableHeld = false;
    bool valid = false;              // false when the antenna factor table is empty or the frequency is unusable
    bool allInRange() const { return valid && !afHeld && !cableHeld; }
};

// Level at fHz through the tables. The cable table may be empty (no cable correction); the antenna-factor table may not.
inline FieldReadout fieldStrengthFromTables(double levelDbm, double fHz, const LossTable& antennaFactor, const LossTable& cableLoss, double preampGainDb = 0.0,
                                            double rOhm = 50.0) {
    FieldReadout r;
    if (antennaFactor.empty() || !(fHz > 0.0) || !std::isfinite(fHz) || !std::isfinite(levelDbm)) { return r; }
    const LookupResult af = antennaFactor.lookup(fHz);
    r.afDb = af.valueDb;
    r.afHeld = !af.inRange();
    if (!cableLoss.empty()) {
        const LookupResult cl = cableLoss.lookup(fHz);
        r.cableLossDb = cl.valueDb;
        r.cableHeld = !cl.inRange();
    }
    FieldStrengthInputs in;
    in.levelDbm = levelDbm;
    in.rOhm = rOhm;
    in.afDb = r.afDb;
    in.cableLossDb = r.cableLossDb;
    in.preampGainDb = preampGainDb;
    r.dbuvPerM = fieldStrengthDbuvPerM(in);
    r.valid = true;
    return r;
}

}  // namespace cascade::core
