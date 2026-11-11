// contest_score.hpp - a contest score model (hamops lane): dupes, QSO points, multipliers per band or once per contest, the running score.
// Pure code; the screen that shows it is another lane's.
//
// WHAT IS AND IS NOT CLAIMED: the model is parameterised (ScoreRules). Two EXAMPLE rule sets are provided for a "simple contest" (zone multipliers
// per band; section multiplier once, two points a QSO). They are NOT claimed to be any sponsor's rules: no sponsor rules page was opened for scoring
// (CQ WW points depend on country and continent and need a country table this lane does not have). A real contest's rules are a rule set built from
// that sponsor's page by whoever wires it. The score is CLAIMED-SCORE material only; the sponsor's log checker computes the real one.
//
// Keys: the call is upper-cased; the band comes from the frequency (ADIF band table, qso_log.hpp); the mode class is the Cabrillo code (CW, PH, FM,
// RY, DG). A QSO with an unusable call, frequency or mode is refused (not counted, not a dupe).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "qso_log.hpp"

namespace cascade::core::ham {

enum class ScopeKind { PerBand, PerBandAndMode, WholeContest };
enum class MultKind { None, PerBand, Once };

struct ScoreRules {
    std::string name;
    int pointsPerQso = 1;
    std::function<int(const Qso&)> points;      // when set, overrides pointsPerQso (return < 0 = the QSO is worth nothing: still logged, not a dupe)
    ScopeKind dupeScope = ScopeKind::PerBand;   // what makes two QSOs with one call the same QSO
    MultKind multKind = MultKind::PerBand;
    int multIndex = 1;                          // which received exchange token is the multiplier (index into Qso::rcvd)
};

// EXAMPLES (see the header comment): not sponsor rules.
inline ScoreRules exampleZoneContest() {
    ScoreRules r;
    r.name = "example: 1 point a QSO, zone multiplier per band, dupes per band";
    r.pointsPerQso = 1;
    r.dupeScope = ScopeKind::PerBand;
    r.multKind = MultKind::PerBand;
    r.multIndex = 1;
    return r;
}
inline ScoreRules exampleSectionContest() {
    ScoreRules r;
    r.name = "example: 2 points a QSO, section multiplier once, one QSO per station";
    r.pointsPerQso = 2;
    r.dupeScope = ScopeKind::WholeContest;
    r.multKind = MultKind::Once;
    r.multIndex = 3;
    return r;
}

enum class ScoreStatus { Counted, Dupe, Refused };

struct QsoOutcome {
    ScoreStatus status = ScoreStatus::Refused;
    int points = 0;
    bool newMultiplier = false;
};

class ContestScore {
public:
    explicit ContestScore(ScoreRules r = ScoreRules()) : rules_(std::move(r)) {}

    void reset() {
        seen_.clear();
        multsPerBand_.clear();
        multsOnce_.clear();
        qsos_ = dupes_ = refused_ = 0;
        points_ = 0;
    }
    const ScoreRules& rules() const { return rules_; }

    // Would this QSO be a dupe of one already counted? (False for an unusable QSO.)
    bool isDupe(const Qso& q) const {
        std::string key;
        return makeKey(q, key) && seen_.count(key) != 0;
    }

    // Is this multiplier value (e.g. a zone) new on this frequency's band? For a once-per-contest rule the band is ignored.
    bool isNewMultiplier(double freqKHz, const std::string& value) const {
        const std::string v = upperAscii(value);
        if (v.empty() || rules_.multKind == MultKind::None) { return false; }
        if (rules_.multKind == MultKind::Once) { return multsOnce_.count(v) == 0; }
        auto it = multsPerBand_.find(bandKey(freqKHz));
        return it == multsPerBand_.end() || it->second.count(v) == 0;
    }

    QsoOutcome add(const Qso& q) {
        QsoOutcome o;
        std::string key;
        if (!makeKey(q, key)) { ++refused_; return o; }
        if (seen_.count(key) != 0) { ++dupes_; o.status = ScoreStatus::Dupe; return o; }
        seen_.insert(key);
        o.status = ScoreStatus::Counted;
        ++qsos_;
        int p = rules_.points ? rules_.points(q) : rules_.pointsPerQso;
        if (p < 0) { p = 0; }
        o.points = p;
        points_ += p;
        if (rules_.multKind != MultKind::None && rules_.multIndex >= 0 && static_cast<std::size_t>(rules_.multIndex) < q.rcvd.size()) {
            const std::string v = upperAscii(q.rcvd[static_cast<std::size_t>(rules_.multIndex)]);
            if (!v.empty()) {
                const bool fresh = (rules_.multKind == MultKind::Once) ? multsOnce_.insert(v).second
                                                                        : multsPerBand_[bandKey(q.freqKHz)].insert(v).second;
                o.newMultiplier = fresh;
            }
        }
        return o;
    }

    std::uint64_t qsos() const { return qsos_; }          // counted (dupes excluded)
    std::uint64_t dupes() const { return dupes_; }
    std::uint64_t refused() const { return refused_; }
    std::uint64_t qsoPoints() const { return points_; }
    std::uint64_t multipliers() const {
        if (rules_.multKind == MultKind::Once) { return multsOnce_.size(); }
        std::uint64_t n = 0;
        for (const auto& kv : multsPerBand_) { n += kv.second.size(); }
        return n;
    }
    std::size_t multipliersOnBand(const std::string& band) const {
        auto it = multsPerBand_.find(band);
        return it == multsPerBand_.end() ? 0 : it->second.size();
    }
    // points x multipliers; with no multiplier rule the points alone.
    std::uint64_t score() const { return rules_.multKind == MultKind::None ? points_ : points_ * multipliers(); }

    static std::string bandKey(double freqKHz) {
        const std::string b = adifBand(freqKHz / 1000.0);
        return b.empty() ? std::string("?") : b;
    }

private:
    bool makeKey(const Qso& q, std::string& key) const {
        std::string call;
        if (!normaliseCall(q.call, call)) { return false; }
        if (!(q.freqKHz > 0.0)) { return false; }
        const std::string mc = cabrilloMode(q.mode);
        if (mc.empty()) { return false; }
        key = call;
        if (rules_.dupeScope != ScopeKind::WholeContest) { key += "|" + bandKey(q.freqKHz); }
        if (rules_.dupeScope == ScopeKind::PerBandAndMode) { key += "|" + mc; }
        return true;
    }

    ScoreRules rules_;
    std::set<std::string> seen_;
    std::map<std::string, std::set<std::string>> multsPerBand_;
    std::set<std::string> multsOnce_;
    std::uint64_t qsos_ = 0, dupes_ = 0, refused_ = 0, points_ = 0;
};

}  // namespace cascade::core::ham
