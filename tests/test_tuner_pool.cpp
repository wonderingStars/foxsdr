// test_tuner_pool.cpp - THE TUNER POOL'S CHOICE (engine serial lane A, row R19): core/tuner_pool.hpp, the pure part.
//
// A radio whose span already covers the frequency is tuned by its VFO alone and wins over one that would have to move its centre; a radio that is not free is never chosen; the lowest number wins
// among equals; a frequency outside every free radio's hardware range, or that is not a frequency, gives no answer; the edge of the span counts the channel's own half width.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <random>

#include "core/tuner_pool.hpp"
#include "test_check.hpp"

using namespace cascade::core::pool;

namespace {
Candidate radio(int n, bool free, double centre, double rate, double lo = 0.0, double hi = 0.0, double ch = 12500.0) {
    Candidate c;
    c.number = n;
    c.free = free;
    c.centreHz = centre;
    c.rateHz = rate;
    c.rangeLoHz = lo;
    c.rangeHiHz = hi;
    c.channelHz = ch;
    return c;
}
}  // namespace

int main() {
    std::printf("test_tuner_pool\n");
    const double MHz = 1e6;
    {
        const std::vector<Candidate> all = {radio(1, true, 100 * MHz, 2 * MHz), radio(2, true, 145 * MHz, 2 * MHz)};
        Pick p = choose(all, 100.3 * MHz);
        CHECK(p.how == Verdict::ByVfo && p.number == 1);              // inside radio 1's span
        p = choose(all, 145.5 * MHz);
        CHECK(p.how == Verdict::ByVfo && p.number == 2);              // inside radio 2's span
        p = choose(all, 433 * MHz);
        CHECK(p.how == Verdict::ByCentre && p.number == 1);           // nobody covers it: the lowest free number retunes
    }
    {   // the VFO beats a retune even when the covering radio has the higher number
        const std::vector<Candidate> all = {radio(1, true, 50 * MHz, 2 * MHz), radio(2, true, 145 * MHz, 2 * MHz)};
        const Pick p = choose(all, 145.2 * MHz);
        CHECK(p.how == Verdict::ByVfo && p.number == 2);
    }
    {   // a radio that is not free is never chosen, even when it covers the frequency
        const std::vector<Candidate> all = {radio(1, false, 100 * MHz, 2 * MHz), radio(2, true, 145 * MHz, 2 * MHz)};
        Pick p = choose(all, 100.3 * MHz);
        CHECK(p.how == Verdict::ByCentre && p.number == 2);
        const std::vector<Candidate> none = {radio(1, false, 100 * MHz, 2 * MHz), radio(2, false, 145 * MHz, 2 * MHz)};
        p = choose(none, 100.3 * MHz);
        CHECK(p.how == Verdict::None && p.number == 0);
    }
    {   // hardware range: radio 1 tunes 24..1766 MHz only
        const std::vector<Candidate> all = {radio(1, true, 100 * MHz, 2 * MHz, 24 * MHz, 1766 * MHz), radio(2, true, 145 * MHz, 2 * MHz, 0.0, 0.0)};
        Pick p = choose(all, 7.1 * MHz);
        CHECK(p.how == Verdict::ByCentre && p.number == 2);           // radio 1 cannot, radio 2 has no known limit
        const std::vector<Candidate> only = {radio(1, true, 100 * MHz, 2 * MHz, 24 * MHz, 1766 * MHz)};
        p = choose(only, 7.1 * MHz);
        CHECK(p.how == Verdict::None);
        p = choose(only, 24 * MHz);
        CHECK(p.how == Verdict::ByCentre);                            // the range edges are in
        p = choose(only, 1766 * MHz);
        CHECK(p.how == Verdict::ByCentre);
        p = choose(only, 1766 * MHz + 1.0);
        CHECK(p.how == Verdict::None);
    }
    {   // the edge of the span: |offset| + half the channel must fit in 0.45 of the rate
        const std::vector<Candidate> all = {radio(1, true, 100 * MHz, 2 * MHz, 0, 0, 100000.0)};
        CHECK(choose(all, 100 * MHz + 0.85 * MHz).how == Verdict::ByVfo);     // 850 kHz + 50 kHz = 900 kHz = 0.45 x 2 MHz
        CHECK(choose(all, 100 * MHz + 0.86 * MHz).how == Verdict::ByCentre);  // a little past it
        CHECK(choose(all, 100 * MHz - 0.85 * MHz).how == Verdict::ByVfo);     // below the centre, the same
        CHECK(choose({radio(1, true, 100 * MHz, 0.0)}, 100 * MHz).how == Verdict::ByCentre);   // an unknown rate covers nothing
    }
    {   // not a frequency
        const std::vector<Candidate> all = {radio(1, true, 100 * MHz, 2 * MHz)};
        for (const double bad : {0.0, -1.0, std::nan(""), HUGE_VAL}) { CHECK(choose(all, bad).how == Verdict::None); }
        CHECK(choose({}, 100 * MHz).how == Verdict::None);
    }
    {   // determinism and the lowest-number rule over random pools: the answer is always a free radio, a covering one when any covers, and the same on a second call
        std::mt19937_64 rng(11);
        for (int i = 0; i < 5000; ++i) {
            std::vector<Candidate> all;
            const int n = 1 + static_cast<int>(rng() % 8);
            for (int k = 1; k <= n; ++k) { all.push_back(radio(k, (rng() % 3) != 0, (50 + static_cast<double>(rng() % 900)) * MHz, (1 + static_cast<double>(rng() % 3)) * MHz)); }
            const double hz = (50 + static_cast<double>(rng() % 900)) * MHz;
            const Pick a = choose(all, hz), b = choose(all, hz);
            CHECK(a.how == b.how && a.number == b.number);
            bool anyFree = false, anyCover = false;
            int lowestCover = 0, lowestFree = 0;
            for (const Candidate& c : all) {
                if (c.free) { anyFree = true; if (lowestFree == 0) { lowestFree = c.number; } }
                if (c.free && coversByVfo(c, hz)) { anyCover = true; if (lowestCover == 0) { lowestCover = c.number; } }
            }
            if (!anyFree) { CHECK(a.how == Verdict::None); }
            else if (anyCover) { CHECK(a.how == Verdict::ByVfo && a.number == lowestCover); }
            else { CHECK(a.how == Verdict::ByCentre && a.number == lowestFree); }
        }
    }
    return testSummary("test_tuner_pool");
}
