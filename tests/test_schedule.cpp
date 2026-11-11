// test_schedule.cpp - TIMED RECORDINGS, the pure clock (core/schedule.*). Time is an argument; nothing here sleeps.
//
//   S1  OCCURRENCE ARITHMETIC: once, daily and weekly, before the first start, on it, between two, at the second.
//   S2  VALIDATION: every field, one rule at a time.
//   S3  THE FILE: damaged entries dropped, values clamped, ids made unique, the count capped.
//   S4  OVERLAP: the same radio only; back-to-back is not an overlap; repeats against repeats over the two-week horizon.
//   S5  THE CLOCK: on time; stops at its length; up to 30 s late still starts, 31 s late is MISSED and never started; a start that fell inside
//       a closed program is counted, not run; many missed repeats are counted once; the clock jumping forward over a start; set back by more
//       than 2 s during a recording (stops) or less (goes on); set back after a run (never runs it again); a restart (the saved state) neither
//       re-runs nor re-counts; a refused start uses the occurrence up; disabling or removing ends a run; run now; skip next.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "core/schedule.hpp"
#include "test_check.hpp"

using namespace cascade::core::sched;

namespace {

constexpr std::int64_t T0 = 1790000000;   // a fixed "now" in 2026 (UTC seconds)

Entry make(std::uint32_t id, int radio, std::int64_t start, int len, Repeat rep = Repeat::Once) {
    Entry e;
    e.id = id;
    e.name = "e" + std::to_string(id);
    e.radio = radio;
    e.startUnix = start;
    e.lengthS = len;
    e.repeat = rep;
    e.recordAudio = true;
    e.createdUnix = T0 - 1000;
    return e;
}

bool has(const std::vector<Decision>& d, Decision::Kind k, std::uint32_t id) {
    for (const Decision& x : d) {
        if (x.kind == k && x.id == id) { return true; }
    }
    return false;
}
const Decision* get(const std::vector<Decision>& d, Decision::Kind k, std::uint32_t id) {
    for (const Decision& x : d) {
        if (x.kind == k && x.id == id) { return &x; }
    }
    return nullptr;
}
std::int64_t ms(std::int64_t s) { return s * 1000; }

void testArithmetic() {
    Entry once = make(1, 1, T0 + 100, 60);
    CHECK(latestStartAtOrBefore(once, T0 + 99) == -1);
    CHECK(latestStartAtOrBefore(once, T0 + 100) == T0 + 100);
    CHECK(latestStartAtOrBefore(once, T0 + 99999) == T0 + 100);
    CHECK(nextStartAfter(once, T0) == T0 + 100);
    CHECK(nextStartAfter(once, T0 + 99) == T0 + 100);
    CHECK(nextStartAfter(once, T0 + 100) == -1);
    Entry daily = make(2, 1, T0 + 100, 60, Repeat::Daily);
    CHECK(latestStartAtOrBefore(daily, T0 + 99) == -1);
    CHECK(latestStartAtOrBefore(daily, T0 + 100) == T0 + 100);
    CHECK(latestStartAtOrBefore(daily, T0 + 100 + 86399) == T0 + 100);
    CHECK(latestStartAtOrBefore(daily, T0 + 100 + 86400) == T0 + 100 + 86400);
    CHECK(latestStartAtOrBefore(daily, T0 + 100 + 10 * 86400 + 5) == T0 + 100 + 10 * 86400);
    CHECK(nextStartAfter(daily, T0 + 100) == T0 + 100 + 86400);
    CHECK(nextStartAfter(daily, T0 + 100 + 86399) == T0 + 100 + 86400);
    CHECK(nextStartAfter(daily, T0) == T0 + 100);
    Entry weekly = make(3, 1, T0 + 100, 60, Repeat::Weekly);
    CHECK(latestStartAtOrBefore(weekly, T0 + 100 + 6 * 86400) == T0 + 100);
    CHECK(latestStartAtOrBefore(weekly, T0 + 100 + 7 * 86400) == T0 + 100 + 7 * 86400);
    CHECK(nextStartAfter(weekly, T0 + 100 + 7 * 86400) == T0 + 100 + 14 * 86400);
    // unhandled counts
    CHECK(unhandledCount(once, T0 + 99) == 0);
    CHECK(unhandledCount(once, T0 + 100) == 1);
    once.lastStartUnix = T0 + 100;
    CHECK(unhandledCount(once, T0 + 99999) == 0);
    daily.lastStartUnix = 0;
    CHECK(unhandledCount(daily, T0 + 100 + 2 * 86400) == 3);   // day 0, 1 and 2
    daily.lastStartUnix = T0 + 100 + 86400;
    CHECK(unhandledCount(daily, T0 + 100 + 2 * 86400) == 1);
    daily.lastStartUnix = 0;
    CHECK(unhandledCount(daily, T0 + 100 + 5000 * 86400LL) == kMaxMissedCount + 1);   // capped
}

void testValidation() {
    const std::int64_t now = T0;
    Entry e = make(1, 1, now + 60, 60);
    CHECK(validate(e, now).empty());
    Entry x = e; x.radio = 0;           CHECK(!validate(x, now).empty());
    x = e; x.radio = 9;                 CHECK(!validate(x, now).empty());
    x = e; x.lengthS = 0;               CHECK(!validate(x, now).empty());
    x = e; x.lengthS = kMaxLengthS + 1; CHECK(!validate(x, now).empty());
    x = e; x.lengthS = kMaxLengthS;     CHECK(validate(x, now).empty());
    x = e; x.lengthS = kMinLengthS;     CHECK(validate(x, now).empty());
    x = e; x.startUnix = 5;             CHECK(!validate(x, now).empty());
    x = e; x.startUnix = 5000000000LL;  CHECK(!validate(x, now).empty());
    x = e; x.startUnix = now - 3600;    CHECK(validate(x, now) == "the start time has already passed");   // a ONCE entry whose whole slot is over
    x = e; x.startUnix = now - 10;      CHECK(validate(x, now).empty());                                    // still inside its length: allowed (it starts at once or is missed)
    x = e; x.startUnix = now - 3600; x.repeat = Repeat::Daily; CHECK(validate(x, now).empty());             // a repeat may have started long ago
    x = e; x.name = std::string(41, 'n'); CHECK(!validate(x, now).empty());
    x = e; x.name = std::string(40, 'n'); CHECK(validate(x, now).empty());
    x = e; x.name = "bad\x01name";      CHECK(!validate(x, now).empty());
    x = e; x.recordAudio = false; x.recordIq = false; CHECK(!validate(x, now).empty());
    x = e; x.recordAudio = false; x.recordIq = true;  CHECK(validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = 0.0;     CHECK(!validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = -5.0;    CHECK(!validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = 7.0e9;   CHECK(!validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = 137.9e6; CHECK(validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = std::nan(""); CHECK(!validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = 137.9e6; x.mode = 8; CHECK(!validate(x, now).empty());   // RAW is not a mode a schedule can ask for
    x = e; x.tune = true; x.freqHz = 137.9e6; x.mode = 1; x.bandwidthHz = 40000.0; CHECK(validate(x, now).empty());
    x = e; x.tune = true; x.freqHz = 137.9e6; x.bandwidthHz = -1.0; CHECK(!validate(x, now).empty());
    x = e; x.repeat = static_cast<Repeat>(7); CHECK(!validate(x, now).empty());
}

void testFile() {
    std::vector<Entry> in;
    in.push_back(make(5, 1, T0 + 10, 60));
    in.push_back(make(5, 2, T0 + 20, 60));       // the same id: renumbered
    in.push_back(make(0, 3, T0 + 30, 60));       // no id: given one
    Entry bad = make(9, 99, T0, 60);             // radio 99: dropped
    in.push_back(bad);
    Entry longName = make(10, 1, T0 + 40, 60);
    longName.name = std::string(100, 'a') + "\n";
    in.push_back(longName);                      // name cut, control characters made spaces
    Entry odd = make(11, 1, T0 + 50, 60);
    odd.missed = 123456; odd.lastOutcome = static_cast<Outcome>(77); odd.lastStartUnix = -5; odd.freqHz = 99.0; odd.mode = 3;
    in.push_back(odd);                           // clamped; the tuning fields of an entry that does not tune are cleared
    const std::vector<Entry> out = sanitise(in);
    CHECK(out.size() == 5);
    std::set<std::uint32_t> ids;
    for (const Entry& e : out) { ids.insert(e.id); CHECK(e.id != 0); }
    CHECK(ids.size() == out.size());
    CHECK(out[0].id == 5 && out[1].id != 5 && out[1].radio == 2);
    CHECK(out[3].name.size() == static_cast<std::size_t>(kMaxNameChars));
    CHECK(out[4].missed == kMaxMissedCount && out[4].lastOutcome == Outcome::None && out[4].lastStartUnix == 0 && out[4].freqHz == 0.0 && out[4].mode == 0);
    // more than the cap
    std::vector<Entry> many;
    for (int i = 0; i < 100; ++i) { many.push_back(make(static_cast<std::uint32_t>(i + 1), 1, T0 + i, 5)); }
    CHECK(static_cast<int>(sanitise(many).size()) == kMaxEntries);
    // a ONCE entry from the past is kept by the file reader (its outcome is part of the record)
    CHECK(sanitise({make(1, 1, T0 - 100000, 60)}).size() == 1);
}

void testOverlap() {
    const Entry a = make(1, 1, T0 + 1000, 600);
    CHECK(overlaps(a, make(2, 1, T0 + 1300, 600), T0));          // starts inside
    CHECK(overlaps(a, make(2, 1, T0 + 500, 600), T0));           // ends inside
    CHECK(overlaps(a, make(2, 1, T0 + 1100, 100), T0));          // inside
    CHECK(overlaps(a, make(2, 1, T0 + 500, 3000), T0));          // contains
    CHECK(!overlaps(a, make(2, 1, T0 + 1600, 600), T0));         // back to back: a ends at 1600, b starts at 1600
    CHECK(!overlaps(a, make(2, 1, T0 + 400, 600), T0));          // b ends at 1000 exactly
    CHECK(!overlaps(a, make(2, 2, T0 + 1300, 600), T0));         // another radio
    CHECK(!overlaps(a, make(2, 1, T0 + 50000, 600), T0));        // far later, once
    // repeats
    const Entry d = make(3, 1, T0 + 1000, 600, Repeat::Daily);
    CHECK(overlaps(d, make(4, 1, T0 + 1300 + 3 * 86400, 60), T0));                    // a once-entry on day 3 lands on a daily run
    CHECK(!overlaps(d, make(4, 1, T0 + 5000 + 3 * 86400, 60), T0));
    CHECK(overlaps(d, make(5, 1, T0 + 1300, 600, Repeat::Weekly), T0));               // a weekly on the same hour
    CHECK(!overlaps(d, make(5, 1, T0 + 2000, 600, Repeat::Weekly), T0));
    // beyond the two-week horizon is not looked at
    CHECK(!overlaps(a, make(2, 1, T0 + 1300 + 20 * 86400, 600), T0));
    // symmetric
    CHECK(overlaps(make(2, 1, T0 + 1300, 600), a, T0) == overlaps(a, make(2, 1, T0 + 1300, 600), T0));
    // a run already in progress at `from`
    CHECK(overlaps(a, make(2, 1, T0 + 1200, 60), T0 + 1100));
}

void testClock() {
    // ---- on time, stops at its length ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60)});
        CHECK(r.tick(ms(T0 + 99)).empty());
        std::vector<Decision> d = r.tick(ms(T0 + 100));
        CHECK(d.size() == 1 && has(d, Decision::Kind::Start, 1) && d[0].occurrence == T0 + 100);
        r.started(1, T0 + 100, ms(T0 + 100));
        CHECK(r.active(1));
        CHECK(r.tick(ms(T0 + 130)).empty());
        CHECK(r.tick(ms(T0 + 159) + 999).empty());
        d = r.tick(ms(T0 + 160));
        CHECK(d.size() == 1 && has(d, Decision::Kind::Stop, 1));
        r.ended(1);
        CHECK(!r.active(1));
        CHECK(r.tick(ms(T0 + 100000)).empty());       // a ONCE entry never again
        CHECK(r.entries()[0].lastOutcome == Outcome::Ran && r.entries()[0].lastStartUnix == T0 + 100);
    }
    // ---- up to 30 s late still starts; 31 s late is missed ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600)});
        const std::vector<Decision> d = r.tick(ms(T0 + 130));
        CHECK(d.size() == 1 && has(d, Decision::Kind::Start, 1));
        Runner q;
        q.setEntries({make(1, 1, T0 + 100, 600)});
        const std::vector<Decision> m = q.tick(ms(T0 + 131));
        CHECK(m.size() == 1 && has(m, Decision::Kind::Missed, 1) && !has(m, Decision::Kind::Start, 1));
        CHECK(q.entries()[0].lastOutcome == Outcome::Missed && q.entries()[0].missed == 1);
        CHECK(q.tick(ms(T0 + 140)).empty() && q.tick(ms(T0 + 100000)).empty());   // never started after the fact
        CHECK(q.dirty());
    }
    // ---- the program was closed over a start: the first look after loading counts it ----
    {
        Entry e = make(1, 1, T0 + 100, 60, Repeat::Daily);
        e.lastStartUnix = T0 + 100;                      // the first day ran before the program closed
        Runner r;
        r.setEntries({e});
        // reopened 3 days and 1 hour later: days 1, 2 and 3 were missed (3 occurrences), none is started
        const std::vector<Decision> d = r.tick(ms(T0 + 100 + 3 * 86400 + 3600));
        CHECK(d.size() == 1 && has(d, Decision::Kind::Missed, 1) && get(d, Decision::Kind::Missed, 1)->count == 3);
        CHECK(r.entries()[0].missed == 3);
        CHECK(r.tick(ms(T0 + 100 + 3 * 86400 + 3601)).empty());
        // and the next day's run still happens
        const std::vector<Decision> n = r.tick(ms(T0 + 100 + 4 * 86400));
        CHECK(n.size() == 1 && has(n, Decision::Kind::Start, 1));
        // opened 10 s into a run that began while it was closed: within the grace, so it starts (and the earlier days are missed)
        Runner s;
        e.lastStartUnix = T0 + 100;
        s.setEntries({e});
        const std::vector<Decision> late = s.tick(ms(T0 + 100 + 2 * 86400 + 10));
        CHECK(has(late, Decision::Kind::Start, 1) && get(late, Decision::Kind::Missed, 1) != nullptr && get(late, Decision::Kind::Missed, 1)->count == 1);
    }
    // ---- a clock that jumps forward over a start misses it; over a whole recording it ends it ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60), make(2, 1, T0 + 5000, 60)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        const std::vector<Decision> d = r.tick(ms(T0 + 4000));    // jumped an hour ahead
        CHECK(has(d, Decision::Kind::Stop, 1));
        r.ended(1);
        CHECK(r.tick(ms(T0 + 9000)).size() == 1 && has(r.tick(ms(T0 + 9000)), Decision::Kind::Missed, 2) == false);   // (second look: already counted)
    }
    // ---- the clock set back during a recording ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        CHECK(r.tick(ms(T0 + 100) - 1500).empty());               // 1.5 s back: the recording goes on
        CHECK(r.tick(ms(T0 + 300)).empty());
        const std::vector<Decision> d = r.tick(ms(T0 + 100) - 2500);   // 2.5 s back: it ends
        CHECK(d.size() == 1 && has(d, Decision::Kind::Stop, 1) && d[0].why.find("set back") != std::string::npos);
        r.ended(1);
        // and the occurrence is not run again when the clock comes forward to the same start
        CHECK(r.tick(ms(T0 + 100)).empty() && r.tick(ms(T0 + 110)).empty());
    }
    // ---- a clock set back after a run never re-runs it (daily) ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60, Repeat::Daily)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        r.tick(ms(T0 + 160));
        r.ended(1);
        CHECK(r.tick(ms(T0 + 50)).empty());
        CHECK(r.tick(ms(T0 + 100)).empty());
        CHECK(r.tick(ms(T0 + 120)).empty());
        CHECK(has(r.tick(ms(T0 + 100 + 86400)), Decision::Kind::Start, 1));
    }
    // ---- a restart: the saved lastStartUnix means nothing is run or counted twice ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60, Repeat::Daily)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        r.tick(ms(T0 + 160));
        r.ended(1);
        const Entry saved = r.entries()[0];               // what the settings file holds
        Runner again;
        again.setEntries({saved});
        CHECK(again.tick(ms(T0 + 170)).empty());          // nothing replayed
        CHECK(again.tick(ms(T0 + 100 + 86400 + 10)).size() == 1);
        CHECK(again.entries()[0].missed == 0);
    }
    // ---- a refused start uses the occurrence up ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600, Repeat::Daily)});
        const std::vector<Decision> d = r.tick(ms(T0 + 100));
        CHECK(has(d, Decision::Kind::Start, 1));
        r.refused(1, T0 + 100);
        CHECK(!r.active(1) && r.entries()[0].lastOutcome == Outcome::Refused);
        CHECK(r.tick(ms(T0 + 101)).empty() && r.tick(ms(T0 + 200)).empty());
        CHECK(has(r.tick(ms(T0 + 100 + 86400)), Decision::Kind::Start, 1));
    }
    // ---- disabling or removing ends a run; a disabled entry never starts ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600), make(2, 1, T0 + 100000, 60)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        std::vector<Entry> e = r.entries();
        e[0].enabled = false;
        r.setEntries(e);
        CHECK(has(r.tick(ms(T0 + 110)), Decision::Kind::Stop, 1));
        r.ended(1);
        e[0].enabled = true;
        e.erase(e.begin());
        Runner q;
        q.setEntries({make(1, 1, T0 + 100, 600)});
        q.tick(ms(T0 + 100));
        q.started(1, T0 + 100, ms(T0 + 100));
        q.setEntries({});
        CHECK(has(q.tick(ms(T0 + 101)), Decision::Kind::Stop, 1));
        Entry off = make(3, 1, T0 + 100, 60);
        off.enabled = false;
        Runner z;
        z.setEntries({off});
        CHECK(z.tick(ms(T0 + 100)).empty() && z.tick(ms(T0 + 500)).empty());
    }
    // ---- another radio is moved to: the run ends ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600)});
        r.tick(ms(T0 + 100));
        r.started(1, T0 + 100, ms(T0 + 100));
        std::vector<Entry> e = r.entries();
        e[0].radio = 2;
        r.setEntries(e);
        CHECK(has(r.tick(ms(T0 + 101)), Decision::Kind::Stop, 1));
    }
    // ---- run now, skip next ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100000, 60, Repeat::Daily)});
        CHECK(r.runNow(1));
        std::vector<Decision> d = r.tick(ms(T0));
        CHECK(d.size() == 1 && has(d, Decision::Kind::Start, 1) && d[0].occurrence == 0);
        r.started(1, 0, ms(T0));
        CHECK(!r.runNow(1));                                     // already running
        CHECK(r.entries()[0].lastStartUnix == 0);                // a manual run does not consume the schedule
        d = r.tick(ms(T0 + 61));
        CHECK(has(d, Decision::Kind::Stop, 1));
        r.ended(1);
        CHECK(!r.runNow(77));
        // skip: the next occurrence passes without a start
        CHECK(r.skipNext(1, T0));
        CHECK(r.entries()[0].lastOutcome == Outcome::Skipped && r.entries()[0].lastStartUnix == T0 + 100000);
        CHECK(r.tick(ms(T0 + 100000)).empty());
        CHECK(has(r.tick(ms(T0 + 100000 + 86400)), Decision::Kind::Start, 1));
        Runner o;
        o.setEntries({make(1, 1, T0 + 100, 60)});
        o.tick(ms(T0 + 100));
        o.started(1, T0 + 100, ms(T0 + 100));
        o.tick(ms(T0 + 200));
        o.ended(1);
        CHECK(!o.skipNext(1, T0 + 300));                          // a ONCE entry that has run has no next occurrence
        CHECK(!o.skipNext(55, T0));
    }
    // ---- two entries on different radios at the same second both start ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60), make(2, 2, T0 + 100, 60)});
        const std::vector<Decision> d = r.tick(ms(T0 + 100));
        CHECK(d.size() == 2 && has(d, Decision::Kind::Start, 1) && has(d, Decision::Kind::Start, 2));
    }
    // ---- a Start decision is given once even when the engine has not answered yet ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 600)});
        CHECK(r.tick(ms(T0 + 100)).size() == 1);
        CHECK(r.tick(ms(T0 + 101)).empty());
    }
    // ---- fractional milliseconds and negative clocks do not misbehave ----
    {
        Runner r;
        r.setEntries({make(1, 1, T0 + 100, 60)});
        CHECK(r.tick(-5000).empty());
        CHECK(r.tick(ms(T0 + 100) - 1).empty());
        CHECK(r.tick(ms(T0 + 100)).size() == 1);
    }
}

}  // namespace

int main() {
    testArithmetic();
    testValidation();
    testFile();
    testOverlap();
    testClock();
    return testSummary("test_schedule");
}
