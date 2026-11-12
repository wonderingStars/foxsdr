// test_band_map.cpp - the band-map store with a spot stack (core/band_map.hpp).
//
//   P1  adding and merging: a new spot, the same call within the tolerance (merge: newest frequency, count), the same call far away (a second spot).
//   P2  the stack: ascending frequency within a band edge range, ties oldest first, hideWorked.
//   P3  age-out: exact boundary, future-stamped spots kept, a refreshed spot survives.
//   P4  bounded size: the oldest-seen goes first, the count never passes the capacity.
//   P5  worked / needed flags and next-up / next-down stepping.
//   P6  refusals and empty/zero/negative cases.
//   P7  fuzz against a reference list.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <string>
#include <vector>

#include "core/band_map.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::ham;

namespace {

std::vector<std::string> calls(const std::vector<Spot>& v) {
    std::vector<std::string> r;
    for (const Spot& s : v) { r.push_back(s.call); }
    return r;
}

void p1Adding() {
    BandMap m;
    CHECK(m.addSpot("k1abc", 14025000, "CW", 100) == AddStatus::Added);
    CHECK(m.size() == 1);
    CHECK(m.addSpot("K1ABC", 14025300, "CW", 160) == AddStatus::Merged);     // within 500 Hz
    CHECK(m.size() == 1);
    Spot s = m.all()[0];
    CHECK(s.call == "K1ABC" && s.freqHz == 14025300 && s.seenUnix == 160 && s.firstUnix == 100 && s.count == 2);
    CHECK(m.addSpot("K1ABC", 14025800.1, "CW", 170) == AddStatus::Added);    // 500.1 Hz from the stored one: a second spot
    CHECK(m.size() == 2);
    CHECK(m.addSpot("K1ABC", 14025300, "CW", 10) == AddStatus::Merged);      // an older stamp never moves seen backwards
    CHECK(m.all()[0].seenUnix == 160);
    CHECK(m.addSpot("W2XYZ", 14025300, "SSB", 200) == AddStatus::Added);     // another call on the same frequency is its own spot
    CHECK(m.size() == 3);
    // the exact tolerance edge merges
    BandMap e;
    e.addSpot("K1AA", 7000000, "CW", 1);
    CHECK(e.addSpot("K1AA", 7000500, "CW", 2) == AddStatus::Merged);
    CHECK(e.addSpot("K1AA", 7000000 - 1000, "CW", 3) == AddStatus::Added);   // 1500 Hz from the merged one
}

void p2Stack() {
    BandMap m;
    m.addSpot("C3", 14030000, "CW", 30);
    m.addSpot("A1", 14010000, "CW", 10);
    m.addSpot("B2", 14020000, "CW", 20);
    m.addSpot("D4", 14020000, "CW", 5);        // same frequency as B2, seen earlier: first
    m.addSpot("E5", 7010000, "CW", 40);
    m.addSpot("F6", 14350000, "CW", 41);       // the upper band edge
    CHECK(calls(m.stack(14000000, 14350000)) == (std::vector<std::string>{"A1", "D4", "B2", "C3", "F6"}));
    CHECK(calls(m.stack(14010001, 14029999)) == (std::vector<std::string>{"D4", "B2"}));
    CHECK(calls(m.stack(14010000, 14010000)) == (std::vector<std::string>{"A1"}));       // inclusive edges
    CHECK(m.stack(15000000, 16000000).empty());
    CHECK(m.stack(14350000, 14000000).empty());                                             // an inverted range is empty, not an error
    m.markWorked("B2");
    CHECK(calls(m.stack(14000000, 14350000, true)) == (std::vector<std::string>{"A1", "D4", "C3", "F6"}));
    CHECK(m.all().size() == 6);
}

void p3AgeOut() {
    BandMapConfig c;
    c.maxAgeSec = 100;
    BandMap m(c);
    m.addSpot("A1", 14000000, "CW", 1000);
    m.addSpot("B2", 14001000, "CW", 1050);
    m.addSpot("C3", 14002000, "CW", 5000);     // stamped in the future of the clock used below
    CHECK(m.ageOut(1100) == 0);                // A1 is exactly 100 s old: kept (older than 100 goes)
    CHECK(m.ageOut(1101) == 1);                // A1 is 101 s old
    CHECK(m.size() == 2);
    m.addSpot("B2", 14001000, "CW", 1140);     // refreshed
    CHECK(m.ageOut(1200) == 0);                // B2 is now 60 s old, C3 is in the future
    CHECK(m.ageOut(1241) == 1 && m.size() == 1);
    CHECK(m.all()[0].call == "C3");
    CHECK(m.ageOut(5101) == 1 && m.size() == 0);
    CHECK(m.ageOut(0) == 0);                   // empty store
}

void p4Bounded() {
    BandMapConfig c;
    c.capacity = 5;
    BandMap m(c);
    for (int i = 0; i < 12; ++i) { m.addSpot("K" + std::to_string(i) + "AA", 14000000 + i * 5000.0, "CW", 100 + i); CHECK(m.size() <= 5); }
    CHECK(m.size() == 5 && m.evicted() == 7);
    CHECK(calls(m.all()) == (std::vector<std::string>{"K7AA", "K8AA", "K9AA", "K10AA", "K11AA"}));   // the 5 newest remain
    // a merge refreshes the seen time, so that spot survives the next eviction
    m.addSpot("K7AA", 14000000 + 7 * 5000.0, "CW", 500);
    m.addSpot("NEW1", 14100000, "CW", 501);
    CHECK(m.size() == 5);
    const auto v = calls(m.all());
    CHECK(std::find(v.begin(), v.end(), "K7AA") != v.end());
    CHECK(std::find(v.begin(), v.end(), "K8AA") == v.end());
    // capacity corner cases
    BandMapConfig z;
    z.capacity = 0;
    BandMap one(z);
    one.addSpot("A1", 7000000, "CW", 1);
    one.addSpot("B2", 7001000, "CW", 2);
    CHECK(one.size() == 1 && one.all()[0].call == "B2" && one.config().capacity == 1);
}

void p5FlagsAndStepping() {
    BandMap m;
    m.addSpot("A1", 14010000, "CW", 1);
    m.addSpot("B2", 14020000, "CW", 2);
    m.addSpot("C3", 14030000, "CW", 3);
    m.addSpot("D4", 14040000, "CW", 4);
    CHECK(m.nextUp(14020000)->call == "C3");              // strictly above
    CHECK(m.nextUp(14019990)->call == "B2");
    CHECK(m.nextDown(14020000)->call == "A1");
    CHECK(!m.nextDown(14010000).has_value());
    CHECK(!m.nextUp(14040000).has_value());
    CHECK(m.nextUp(0)->call == "A1" && m.nextDown(1.0e9)->call == "D4");
    CHECK(m.markWorked("c3") == 1);
    CHECK(m.nextUp(14020000)->call == "D4");              // worked spots are skipped...
    CHECK(m.nextUp(14020000, false)->call == "C3");       // ...unless asked not to
    CHECK(m.markWorked("C3", false) == 1 && m.nextUp(14020000)->call == "C3");
    CHECK(m.setNeeded("D4", true) == 1);
    CHECK(m.nextUp(14000000, true, true)->call == "D4");
    CHECK(m.nextDown(14050000, true, true)->call == "D4");
    CHECK(!m.nextDown(14039999, true, true).has_value());
    CHECK(m.markWorked("NOBODY") == 0 && m.setNeeded("NOBODY", true) == 0 && m.markWorked("BAD CALL") == 0);
    // the same call at two places: both are flagged
    m.addSpot("A1", 7010000, "CW", 9);
    CHECK(m.markWorked("A1") == 2);
}

void p6Refusals() {
    BandMap m;
    CHECK(m.addSpot("", 14000000, "CW", 1) == AddStatus::Refused);
    CHECK(m.addSpot("BAD CALL", 14000000, "CW", 1) == AddStatus::Refused);
    CHECK(m.addSpot("K1AA", 0, "CW", 1) == AddStatus::Refused);
    CHECK(m.addSpot("K1AA", -5, "CW", 1) == AddStatus::Refused);
    CHECK(m.addSpot("K1AA", 2.0e12, "CW", 1) == AddStatus::Refused);
    CHECK(m.addSpot("K1AA", 14000000, "CW", -1) == AddStatus::Refused);
    CHECK(m.size() == 0 && m.refused() == 6);
    CHECK(m.all().empty() && !m.nextUp(0).has_value() && !m.nextDown(1e9).has_value());
    m.addSpot("K1AA", 14000000, "AVERYLONGMODENAMEXXXXXXXX", 1);
    CHECK(m.all()[0].mode.size() == 12);
    m.clear();
    CHECK(m.size() == 0);
}

void p7Fuzz() {
    hamops_test::Rng rng(21);
    for (int run = 0; run < 100; ++run) {
        BandMapConfig c;
        c.capacity = 1 + rng.below(30);
        c.maxAgeSec = 50 + rng.below(100);
        BandMap m(c);
        std::int64_t now = 1000;
        for (int i = 0; i < 400; ++i) {
            now += static_cast<std::int64_t>(rng.below(20));
            const int op = static_cast<int>(rng.below(10));
            if (op < 7) {
                m.addSpot("K" + std::to_string(rng.below(40)) + "AB", 14000000 + rng.below(40) * 1000.0, "CW", now);
            } else if (op == 7) {
                m.ageOut(now);
                for (const Spot& s : m.all()) { CHECK(now - s.seenUnix <= c.maxAgeSec); }
            } else if (op == 8) {
                m.markWorked("K" + std::to_string(rng.below(40)) + "AB");
            } else {
                const double f = 14000000 + rng.below(40) * 1000.0;
                const auto up = m.nextUp(f);
                if (up) { CHECK(up->freqHz > f); }
                const auto dn = m.nextDown(f, false);
                if (dn) { CHECK(dn->freqHz < f); }
            }
            CHECK(m.size() <= c.capacity);
            const auto all = m.all();
            CHECK(std::is_sorted(all.begin(), all.end(), [](const Spot& a, const Spot& b) { return a.freqHz < b.freqHz; }));
        }
    }
}

}  // namespace

int main() {
    p1Adding();
    p2Stack();
    p3AgeOut();
    p4Bounded();
    p5FlagsAndStepping();
    p6Refusals();
    p7Fuzz();
    return testSummary("test_band_map");
}
