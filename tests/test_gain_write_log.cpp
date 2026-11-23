// A manual gain write is one plain line in the diagnostics log, at most one per 500 ms per stage, the last value of a burst winning (core/gain_write_log.hpp; user bug A, 2026-11-23).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/gain_write_log.hpp"
#include "test_check.hpp"

using cascade::core::GainWriteLog;

int main() {
    std::printf("gain write log\n");
    {
        GainWriteLog log;
        auto a = log.note("LNA", 16.3, 1000);
        CHECK(a.size() == 1 && a[0] == "source: gain LNA 16.3 dB");   // the first write is logged at once: stage and value, no frequency
        CHECK(a[0].find("Hz") == std::string::npos);
    }
    {   // a dragged slider: forty writes in two seconds, 50 ms apart, to one stage
        GainWriteLog log;
        std::vector<std::string> all;
        std::int64_t t = 0;
        for (int i = 0; i < 40; ++i, t += 50) {
            for (const std::string& l : log.note("TUNER", 10.0 + i * 0.5, t)) { all.push_back(l); }
            for (const std::string& l : log.flush(t)) { all.push_back(l); }
        }
        for (const std::string& l : log.flush(t + 600)) { all.push_back(l); }
        CHECK(all.size() >= 4 && all.size() <= 6);                     // 2 s / 500 ms, plus the first, not forty
        CHECK(all.front() == "source: gain TUNER 10.0 dB");
        CHECK(all.back() == "source: gain TUNER 29.5 dB");             // the LAST value of the drag is the one that ends up in the log
        CHECK(!log.anyHeld());
    }
    {   // inside the gap the value is held, not lost; it comes out once the gap has passed, once
        GainWriteLog log;
        CHECK(log.note("VGA", 5.0, 0).size() == 1);
        CHECK(log.note("VGA", 6.0, 100).empty());
        CHECK(log.note("VGA", 7.0, 200).empty());
        CHECK(log.anyHeld());
        CHECK(log.flush(499).empty());
        const auto out = log.flush(500);
        CHECK(out.size() == 1 && out[0] == "source: gain VGA 7.0 dB");
        CHECK(log.flush(5000).empty());
        CHECK(!log.anyHeld());
    }
    {   // the stages are limited apart: writing LNA does not hold back MIXER
        GainWriteLog log;
        CHECK(log.note("LNA", 1.0, 0).size() == 1);
        CHECK(log.note("MIXER", 2.0, 10).size() == 1);
        CHECK(log.note("LNA", 3.0, 20).empty());
        CHECK(log.note("MIXER", 4.0, 30).empty());
    }
    {   // KEYED BY RADIO AS WELL AS STAGE (review A5): two radios' "TUNER" are two stages, each with its own gap; the first radio's lines are as they always were, the second's name it
        GainWriteLog log;
        const auto first = log.note("TUNER", 10.0, 0, 0);
        const auto second = log.note("TUNER", 20.0, 10, 1);          // inside the first radio's gap, but another radio: logged at once
        CHECK(first.size() == 1 && first[0] == "source: gain TUNER 10.0 dB");
        CHECK(second.size() == 1 && second[0] == "source: gain TUNER 20.0 dB (radio 2)");
        CHECK(log.note("TUNER", 11.0, 20, 0).empty() && log.note("TUNER", 21.0, 30, 1).empty());   // each radio's own gap holds its own writes
        const auto out = log.flush(600);
        CHECK(out.size() == 2);                                       // both held values come out, each as its own radio's
        bool one = false, two = false;
        for (const std::string& l : out) { one = one || l == "source: gain TUNER 11.0 dB"; two = two || l == "source: gain TUNER 21.0 dB (radio 2)"; }
        CHECK(one && two);
    }
    {   // a write after a quiet gap is logged at once
        GainWriteLog log;
        CHECK(log.note("TUNER", 1.0, 0).size() == 1);
        CHECK(log.note("TUNER", 2.0, 700).size() == 1);
    }
    return testSummary("test_gain_write_log");
}
