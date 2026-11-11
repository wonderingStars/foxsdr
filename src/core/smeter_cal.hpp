// S-meter calibration maths: dBFS to dBm with a per-band offset that follows gain changes, and dBm to
// S units by the IARU Region 1 convention. Pure functions and one small value type; no I/O, no
// allocation outside the table's set-up; nothing here is wired into the engine
// (docs/feature-records/signal-blocks.md, "Wiring hand-off", says where it goes). Design:
// pre-release-features-design.md section 15.
//
// THE CONVENTION, AND WHERE IT COMES FROM. IARU Region 1 Technical Recommendation R.1 (Brighton, 1981):
//   * S9 is a receiver input power of -73 dBm for the HF bands (50 uV rms into 50 ohm) and -93 dBm
//     for VHF/UHF (5 uV into 50 ohm);
//   * one S unit is 6 dB (voltage ratio 2, power ratio 4), so S8 is 6 dB under S9 ... S1 is 48 dB
//     under S9 (-121 dBm on HF, -141 dBm on VHF/UHF);
//   * a signal stronger than S9 is given as "S9 + N dB".
// READ in this session: Wikipedia "S meter" and hamwaves.com "Decibel & S-Readings" (https://hamwaves.com/decibel/en/),
// both quoting the recommendation and tabulating S1..S9 and S9+20/+40; they agree with each other and with the
// facts report. THE RECOMMENDATION ITSELF WAS NOT OPENED (its archived copy at algonet.se is not reachable from
// here): UNVERIFIED against the primary document. The recommendation as quoted separates "HF" from "VHF/UHF";
// the 30 MHz boundary is the conventional one and is how the facts report and a FlexRadio forum answer put it;
// whether exactly 30.000000 MHz counts as HF or VHF is NOT stated in anything read, and here it is VHF/UHF
// (f >= 30 MHz). Many analogue meters do not follow 6 dB per S unit; this is the convention, not a claim about
// any receiver.
//
// WHAT CALIBRATION MEANS HERE. The engine measures channel power in dBFS (before the demodulator). A
// calibration says: "with this tuner at this total gain, a known level of N dBm at the antenna connector read
// M dBFS", stored as offset = N - M for a frequency range (a "band"). Afterwards
//     dBm = dBFS + offset - (gainNow - gainAtCalibration)         [gain in dB]
// because raising the gain by G dB raises the dBFS reading of the same antenna signal by G dB. Where the
// driver's gain is not in dB (steps), nothing can be compensated: the calibration then holds only at the gain it
// was made at, and calibratedAt() says so. THIS DOES NOT CHANGE ANY SQUELCH SCALE: the squelch threshold stays on
// the dBFS scale; only the display mapping uses this.
//
// THREADS. Everything here is re-entrant and allocation-free except CalTable::set()/fromText() (control
// thread). A CalTable is read from the engine pass and written from the control thread under whatever lock the
// settings already use; this header takes none.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cascade::core::smeter {

// --- the convention --------------------------------------------------------------------
inline constexpr double kHfVhfBoundaryHz = 30.0e6;
inline constexpr double kS9HfDbm = -73.0;
inline constexpr double kS9VhfDbm = -93.0;
inline constexpr double kDbPerSUnit = 6.0;
inline constexpr double kMaxOverS9Db = 60.0;     // the display saturates at S9 + 60 dB
inline constexpr double kFloorDbm = -200.0;      // the dBFS floor of the power meter is -200 dB
inline constexpr double kCeilingDbm = 30.0;

// S9 for a frequency: -73 dBm below 30 MHz, -93 dBm from 30 MHz up. A non-finite or non-positive frequency is
// treated as HF.
inline double s9Dbm(double frequencyHz) {
    return (std::isfinite(frequencyHz) && frequencyHz >= kHfVhfBoundaryHz) ? kS9VhfDbm : kS9HfDbm;
}

// The decomposition of a level for display.
struct SReading {
    int sUnit = 0;          // 0..9 (S0 is anything below S1 - 6 dB: a floor, not a measurement)
    double overS9Db = 0.0;  // dB above S9, 0 at or below S9; saturates at kMaxOverS9Db
    double units = 0.0;     // one number: 9 + dB/6 at or below S9 (S8 = 8.0, S1 = 1.0, floored at 0); above S9,
                            // 9 + overDb/60 (S9 = 9.0, S9+20 dB = 9.333, S9+60 dB = 10.0)
    bool valid = false;     // false for a NaN level
};

// dBm to S units. A NaN gives {valid = false, S0}. Levels are clamped to [kFloorDbm, kCeilingDbm] first.
inline SReading dbmToS(double dbm, double frequencyHz) {
    SReading r;
    if (std::isnan(dbm)) { return r; }
    dbm = std::clamp(dbm, kFloorDbm, kCeilingDbm);
    const double over = dbm - s9Dbm(frequencyHz);  // dB relative to S9
    r.valid = true;
    if (over > 0.0) {
        r.overS9Db = std::min(over, kMaxOverS9Db);
        r.sUnit = 9;
        r.units = 9.0 + r.overS9Db / 60.0;
    } else {
        const double u = std::max(0.0, 9.0 + over / kDbPerSUnit);
        r.units = u;
        r.sUnit = std::clamp(static_cast<int>(std::floor(u + 0.5)), 0, 9);
    }
    return r;
}

// "S7", "S9", "S9+20 dB" (rounded to the nearest S unit, or to the nearest dB above S9; a fraction of a dB over
// S9 under 0.5 dB reads "S9"). "S?" for a NaN level.
inline std::string formatS(double dbm, double frequencyHz) {
    const SReading r = dbmToS(dbm, frequencyHz);
    if (!r.valid) { return "S?"; }
    char buf[32];
    const long over = std::lround(r.overS9Db);
    if (over >= 1) {
        std::snprintf(buf, sizeof buf, "S9+%ld dB", over);
    } else {
        std::snprintf(buf, sizeof buf, "S%d", r.sUnit);
    }
    return buf;
}

// "-85 dBm" (whole dB, the way a meter caption reads); "-- dBm" for NaN.
inline std::string formatDbm(double dbm) {
    if (std::isnan(dbm)) { return "-- dBm"; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld dBm", std::lround(std::clamp(dbm, kFloorDbm, kCeilingDbm)));
    return buf;
}

// The dBm level the S reading means (the inverse of dbmToS for whole S units): S8 on HF is -79 dBm. `sUnit` 0..9
// below or at S9; a positive `overS9Db` adds to S9.
inline double sToDbm(int sUnit, double overS9Db, double frequencyHz) {
    sUnit = std::clamp(sUnit, 0, 9);
    if (sUnit == 9) { return s9Dbm(frequencyHz) + std::clamp(overS9Db, 0.0, kMaxOverS9Db); }
    return s9Dbm(frequencyHz) - (9 - sUnit) * kDbPerSUnit;
}

// --- calibration ----------------------------------------------------------------------

// One band's calibration: made with a known level `knownDbm` reading `measuredDbfs`, at total tuner gain
// `gainDb` (meaningful only when gainIsDb).
struct CalEntry {
    double loHz = 0.0, hiHz = 0.0;  // [lo, hi): the band this entry covers
    double offsetDb = 0.0;          // known dBm - measured dBFS
    double gainDb = 0.0;            // total gain at the calibration
    bool gainIsDb = true;           // false: the driver's gain is in steps; no compensation is possible
};

// The offset a measurement defines. False (and offset untouched) when either number is not finite or the measured
// level is at the meter's floor (-200 dBFS: nothing was measured), or the known level is outside [-150, +30] dBm.
inline bool offsetFromMeasurement(double knownDbm, double measuredDbfs, double& offsetDb) {
    if (!std::isfinite(knownDbm) || !std::isfinite(measuredDbfs)) { return false; }
    if (measuredDbfs <= -199.0 || knownDbm < -150.0 || knownDbm > kCeilingDbm) { return false; }
    offsetDb = knownDbm - measuredDbfs;
    return true;
}

struct CalResult {
    double dbm = kFloorDbm;
    bool calibrated = false;  // an entry covers this frequency
    bool exact = false;       // calibrated AND the gain is known to be the calibrated gain or compensated in dB
};

// Applies an entry (or none) to a dBFS reading.
inline CalResult applyCal(const CalEntry* e, double dbfs, double gainNowDb) {
    CalResult r;
    if (!std::isfinite(dbfs)) { return r; }
    if (e == nullptr) {
        r.dbm = std::clamp(dbfs, kFloorDbm, kCeilingDbm);  // uncalibrated: the dBFS number stands in as an ESTIMATE
        return r;
    }
    double dbm = dbfs + e->offsetDb;
    r.calibrated = true;
    if (e->gainIsDb) {
        if (std::isfinite(gainNowDb) && std::isfinite(e->gainDb)) {
            dbm -= gainNowDb - e->gainDb;
            r.exact = true;
        }
    } else {
        r.exact = std::isfinite(gainNowDb) && std::isfinite(e->gainDb) && gainNowDb == e->gainDb;
    }
    r.dbm = std::clamp(dbm, kFloorDbm, kCeilingDbm);
    return r;
}

// The calibrations of one DEVICE: at most kMaxEntries bands.
class CalTable {
public:
    static constexpr std::size_t kMaxEntries = 64;

    // Adds an entry, replacing one with the same [lo, hi). False (nothing changed) for a bad range, a non-finite
    // number, an offset beyond +-200 dB, or a full table.
    bool set(const CalEntry& e) {
        if (!valid(e)) { return false; }
        for (CalEntry& x : entries_) {
            if (x.loHz == e.loHz && x.hiHz == e.hiHz) { x = e; return true; }
        }
        if (entries_.size() >= kMaxEntries) { return false; }
        entries_.push_back(e);
        return true;
    }

    // Removes the entry covering `frequencyHz` (the one find() would return). False when none.
    bool clearAt(double frequencyHz) {
        const CalEntry* f = find(frequencyHz);
        if (f == nullptr) { return false; }
        entries_.erase(entries_.begin() + (f - entries_.data()));
        return true;
    }
    void clear() { entries_.clear(); }
    std::size_t size() const { return entries_.size(); }
    const std::vector<CalEntry>& entries() const { return entries_; }

    // The entry whose [lo, hi) contains the frequency; of several, the narrowest. Null for none, or a bad frequency.
    const CalEntry* find(double frequencyHz) const {
        if (!std::isfinite(frequencyHz)) { return nullptr; }
        const CalEntry* best = nullptr;
        for (const CalEntry& e : entries_) {
            if (frequencyHz >= e.loHz && frequencyHz < e.hiHz) {
                if (best == nullptr || (e.hiHz - e.loHz) < (best->hiHz - best->loHz)) { best = &e; }
            }
        }
        return best;
    }

    // dBFS -> dBm at a frequency and the gain now.
    CalResult dbm(double dbfs, double frequencyHz, double gainNowDb) const {
        return applyCal(find(frequencyHz), dbfs, gainNowDb);
    }

    // One entry per line: "lo_hz hi_hz offset_db gain_db gain_is_db(0|1)". Round-trips through fromText().
    std::string toText() const {
        std::string out;
        char buf[160];
        for (const CalEntry& e : entries_) {
            std::snprintf(buf, sizeof buf, "%.0f %.0f %.6f %.6f %d\n", e.loHz, e.hiHz, e.offsetDb, e.gainDb, e.gainIsDb ? 1 : 0);
            out += buf;
        }
        return out;
    }

    // Replaces the table with what the text holds. A line that does not parse, or an entry that is not valid, makes the
    // WHOLE text refused (false, table unchanged): a half-read calibration would be worse than none. Blank lines are
    // ignored. More than kMaxEntries lines are refused.
    bool fromText(const std::string& text) {
        std::vector<CalEntry> parsed;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) { eol = text.size(); }
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
            if (line.empty()) { continue; }
            double lo = 0, hi = 0, off = 0, gain = 0;
            int isDb = 0;
            char extra = 0;
            if (std::sscanf(line.c_str(), "%lf %lf %lf %lf %d %c", &lo, &hi, &off, &gain, &isDb, &extra) != 5) { return false; }
            if (isDb != 0 && isDb != 1) { return false; }
            CalEntry e;
            e.loHz = lo; e.hiHz = hi; e.offsetDb = off; e.gainDb = gain; e.gainIsDb = (isDb == 1);
            if (!valid(e) || parsed.size() >= kMaxEntries) { return false; }
            parsed.push_back(e);
        }
        entries_ = std::move(parsed);
        return true;
    }

private:
    static bool valid(const CalEntry& e) {
        return std::isfinite(e.loHz) && std::isfinite(e.hiHz) && std::isfinite(e.offsetDb) && std::isfinite(e.gainDb) &&
               e.loHz >= 0.0 && e.hiHz > e.loHz && std::fabs(e.offsetDb) <= 200.0 && std::fabs(e.gainDb) <= 200.0;
    }
    std::vector<CalEntry> entries_;
};

}  // namespace cascade::core::smeter
