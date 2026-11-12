// test_contest_score.cpp - the contest score model (core/contest_score.hpp).
//
//   S1  counting: points, dupes per band (the same call on another band counts), multipliers per band.
//   S2  the once-per-contest multiplier and whole-contest dupes (a second band for the same call is a dupe).
//   S3  refusals: bad call, no frequency, a mode without a Cabrillo code: not counted, not a dupe.
//   S4  boundaries: empty, case, the same multiplier on two bands, a band the table does not know, the points callback (negative -> zero).
//   S5  isDupe / isNewMultiplier queries do not change the score; reset().
//   S6  a hand-computed 60-QSO log against the model (an independent calculation in the test).
//   S7  fuzz against a reference implementation written in the test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/contest_score.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::ham;

namespace {

Qso q(const char* call, double kHz, const char* mode, const char* zone) {
    Qso x;
    x.unixTime = 1000;
    x.freqKHz = kHz;
    x.mode = mode;
    x.call = call;
    x.sent = {"599", "05"};
    x.rcvd = {"599", zone};
    return x;
}

void s1Counting() {
    ContestScore s(exampleZoneContest());
    QsoOutcome o = s.add(q("K1AA", 14025, "CW", "05"));
    CHECK(o.status == ScoreStatus::Counted && o.points == 1 && o.newMultiplier);
    o = s.add(q("K1AA", 14030, "CW", "05"));                 // same call, same band: dupe
    CHECK(o.status == ScoreStatus::Dupe && o.points == 0 && !o.newMultiplier);
    o = s.add(q("k1aa", 14030, "CW", "05"));                 // case does not matter
    CHECK(o.status == ScoreStatus::Dupe);
    o = s.add(q("K1AA", 7010, "CW", "05"));                  // another band: counts, and the zone is new on 40 m
    CHECK(o.status == ScoreStatus::Counted && o.newMultiplier);
    o = s.add(q("W2BB", 14040, "CW", "05"));                 // same zone, same band: counts, no new multiplier
    CHECK(o.status == ScoreStatus::Counted && !o.newMultiplier);
    o = s.add(q("W3CC", 14050, "CW", "14"));
    CHECK(o.status == ScoreStatus::Counted && o.newMultiplier);
    CHECK(s.qsos() == 4);
    CHECK(s.dupes() == 2);
    CHECK(s.qsoPoints() == 4);
    CHECK(s.multipliers() == 3);                             // 05 on 20, 05 on 40, 14 on 20
    CHECK(s.multipliersOnBand("20m") == 2 && s.multipliersOnBand("40m") == 1 && s.multipliersOnBand("80m") == 0);
    CHECK(s.score() == 4u * 3u);
}

void s2Once() {
    ContestScore s(exampleSectionContest());
    auto w = [](const char* call, double kHz, const char* sect) {
        Qso x;
        x.freqKHz = kHz; x.mode = "CW"; x.call = call;
        x.rcvd = {"12", "A", "70", sect};
        return x;
    };
    QsoOutcome o = s.add(w("K1AA", 14000, "CT"));
    CHECK(o.status == ScoreStatus::Counted && o.points == 2 && o.newMultiplier);
    o = s.add(w("K1AA", 7000, "CT"));                        // the same station on another band: a dupe in a whole-contest rule
    CHECK(o.status == ScoreStatus::Dupe);
    o = s.add(w("W2BB", 7000, "CT"));                        // CT again, on another band: not new (once per contest)
    CHECK(o.status == ScoreStatus::Counted && !o.newMultiplier);
    o = s.add(w("W3CC", 3500, "MI"));
    CHECK(o.newMultiplier);
    CHECK(s.qsos() == 3 && s.qsoPoints() == 6 && s.multipliers() == 2 && s.score() == 12);
    CHECK(!s.isNewMultiplier(14000, "ct") && s.isNewMultiplier(14000, "WMA"));
}

void s3Refusals() {
    ContestScore s(exampleZoneContest());
    CHECK(s.add(q("BAD CALL", 14000, "CW", "05")).status == ScoreStatus::Refused);
    CHECK(s.add(q("", 14000, "CW", "05")).status == ScoreStatus::Refused);
    CHECK(s.add(q("K1AA", 0, "CW", "05")).status == ScoreStatus::Refused);
    CHECK(s.add(q("K1AA", -4, "CW", "05")).status == ScoreStatus::Refused);
    CHECK(s.add(q("K1AA", 14000, "WFM", "05")).status == ScoreStatus::Refused);
    CHECK(s.add(q("K1AA", 14000, "", "05")).status == ScoreStatus::Refused);
    CHECK(s.refused() == 6 && s.qsos() == 0 && s.dupes() == 0 && s.score() == 0);
    CHECK(!s.isDupe(q("K1AA", 14000, "WFM", "05")));
    // after the refusals the same call is still new
    CHECK(s.add(q("K1AA", 14000, "CW", "05")).status == ScoreStatus::Counted);
}

void s4Boundaries() {
    ContestScore e(exampleZoneContest());
    CHECK(e.score() == 0 && e.qsos() == 0 && e.multipliers() == 0);
    // a multiplier token that is missing or empty: counted, no multiplier
    Qso a = q("K1AA", 14000, "CW", "");
    CHECK(e.add(a).status == ScoreStatus::Counted && e.multipliers() == 0);
    Qso b = q("K1BB", 14000, "CW", "05");
    b.rcvd.resize(1);
    CHECK(e.add(b).status == ScoreStatus::Counted && e.multipliers() == 0);
    // a frequency in no band: still scored, under the "?" band
    CHECK(e.add(q("K1CC", 5000, "CW", "05")).newMultiplier);
    CHECK(e.add(q("K1CC", 5020, "CW", "05")).status == ScoreStatus::Dupe);   // same unknown band
    // dupes per band AND mode
    ScoreRules r = exampleZoneContest();
    r.dupeScope = ScopeKind::PerBandAndMode;
    ContestScore m(r);
    CHECK(m.add(q("K1AA", 14000, "CW", "05")).status == ScoreStatus::Counted);
    CHECK(m.add(q("K1AA", 14200, "USB", "05")).status == ScoreStatus::Counted);   // PH on the same band is another QSO
    CHECK(m.add(q("K1AA", 14210, "LSB", "05")).status == ScoreStatus::Dupe);      // PH again
    // no multiplier rule: score = points
    ScoreRules n;
    n.multKind = MultKind::None;
    n.pointsPerQso = 3;
    ContestScore p(n);
    p.add(q("K1AA", 14000, "CW", "05"));
    p.add(q("K1BB", 14000, "CW", "05"));
    CHECK(p.score() == 6 && p.multipliers() == 0);
    // the points callback; a negative value counts as zero but the QSO is logged and its call is not free again
    ScoreRules cb = exampleZoneContest();
    cb.points = [](const Qso& x) { return x.freqKHz < 10000 ? 3 : -1; };
    ContestScore c(cb);
    CHECK(c.add(q("K1AA", 7000, "CW", "05")).points == 3);
    CHECK(c.add(q("K1BB", 14000, "CW", "05")).points == 0);
    CHECK(c.qsos() == 2 && c.qsoPoints() == 3);
    CHECK(c.add(q("K1BB", 14001, "CW", "05")).status == ScoreStatus::Dupe);
}

void s5QueriesAndReset() {
    ContestScore s(exampleZoneContest());
    s.add(q("K1AA", 14000, "CW", "05"));
    const auto before = s.score();
    CHECK(s.isDupe(q("K1AA", 14100, "CW", "09")));
    CHECK(!s.isDupe(q("K1AA", 7000, "CW", "09")));
    CHECK(!s.isNewMultiplier(14000, "05") && s.isNewMultiplier(14000, "06") && s.isNewMultiplier(7000, "05") && !s.isNewMultiplier(14000, ""));
    CHECK(s.score() == before && s.qsos() == 1 && s.dupes() == 0);
    s.reset();
    CHECK(s.score() == 0 && s.qsos() == 0 && s.multipliers() == 0);
    CHECK(s.add(q("K1AA", 14000, "CW", "05")).status == ScoreStatus::Counted);
}

void s6HandLog() {
    // 60 QSOs: 20 stations x 3 bands, zones cycling over 4 values; every third QSO is repeated (a dupe).
    ContestScore s(exampleZoneContest());
    const double bands[3] = {14000, 7000, 21000};
    const char* zones[4] = {"01", "02", "03", "04"};
    int counted = 0, dupes = 0;
    for (int b = 0; b < 3; ++b) {
        for (int i = 0; i < 20; ++i) {
            const std::string call = "K" + std::to_string(i) + "AA";
            const std::string zone = zones[i % 4];
            Qso x = q(call.c_str(), bands[b] + i, "CW", zone.c_str());
            CHECK(s.add(x).status == ScoreStatus::Counted);
            ++counted;
            if (i % 3 == 0) { CHECK(s.add(x).status == ScoreStatus::Dupe); ++dupes; }
        }
    }
    CHECK(s.qsos() == static_cast<std::uint64_t>(counted) && counted == 60);
    CHECK(s.dupes() == static_cast<std::uint64_t>(dupes) && dupes == 21);
    CHECK(s.multipliers() == 12);          // 4 zones x 3 bands
    CHECK(s.score() == 60u * 12u);
}

void s7Fuzz() {
    hamops_test::Rng rng(11);
    for (int run = 0; run < 200; ++run) {
        ScoreRules r = exampleZoneContest();
        const int dupeMode = static_cast<int>(rng.below(3));
        r.dupeScope = dupeMode == 0 ? ScopeKind::PerBand : dupeMode == 1 ? ScopeKind::PerBandAndMode : ScopeKind::WholeContest;
        const int mm = static_cast<int>(rng.below(3));
        r.multKind = mm == 0 ? MultKind::None : mm == 1 ? MultKind::PerBand : MultKind::Once;
        r.pointsPerQso = static_cast<int>(rng.below(5));
        ContestScore s(r);
        // reference model
        std::set<std::string> seen;
        std::map<std::string, std::set<std::string>> mult;
        std::set<std::string> once;
        std::uint64_t qsos = 0, dup = 0, pts = 0, ref = 0;
        const double fr[4] = {1850, 3550, 7050, 14050};
        const char* md[3] = {"CW", "USB", "FT8"};
        for (int i = 0; i < 300; ++i) {
            const std::string call = "K" + std::to_string(rng.below(25)) + "ZZ";
            const double f = fr[rng.below(4)];
            const std::string mode = md[rng.below(3)];
            const std::string zone = std::to_string(rng.below(6));
            const std::string band = ContestScore::bandKey(f);
            const std::string mc = cabrilloMode(mode);
            std::string key = call;
            if (r.dupeScope != ScopeKind::WholeContest) { key += "|" + band; }
            if (r.dupeScope == ScopeKind::PerBandAndMode) { key += "|" + mc; }
            const QsoOutcome o = s.add(q(call.c_str(), f, mode.c_str(), zone.c_str()));
            if (seen.count(key)) { CHECK(o.status == ScoreStatus::Dupe); ++dup; continue; }
            seen.insert(key);
            CHECK(o.status == ScoreStatus::Counted);
            ++qsos;
            pts += static_cast<std::uint64_t>(r.pointsPerQso);
            bool fresh = false;
            if (r.multKind == MultKind::PerBand) { fresh = mult[band].insert(zone).second; }
            if (r.multKind == MultKind::Once) { fresh = once.insert(zone).second; }
            CHECK(o.newMultiplier == fresh);
        }
        std::uint64_t mcount = 0;
        if (r.multKind == MultKind::PerBand) { for (auto& kv : mult) { mcount += kv.second.size(); } }
        if (r.multKind == MultKind::Once) { mcount = once.size(); }
        ref = r.multKind == MultKind::None ? pts : pts * mcount;
        CHECK(s.qsos() == qsos && s.dupes() == dup && s.qsoPoints() == pts && s.multipliers() == mcount && s.score() == ref);
    }
}

}  // namespace

int main() {
    s1Counting();
    s2Once();
    s3Refusals();
    s4Boundaries();
    s5QueriesAndReset();
    s6HandLog();
    s7Fuzz();
    return testSummary("test_contest_score");
}
