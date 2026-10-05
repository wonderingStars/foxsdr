// core/phase_clock.hpp - the stopwatch whose sentence tells a two-minute plugin
// rescan WHICH of its six steps it spent the two minutes in.
//
// The log of the session that prompted it had two lines 119.976 s apart and
// nothing between them: the line that opens a rescan, and the first line of the
// plugins it loaded. The clock is injected here, so every figure below is one a
// test chose rather than one it waited for.
//
// WHAT IS HELD: laps are summed in the order they were first opened and a name
// that comes round again ADDS to its lap (a sequence that visits a step twice
// reads as one figure for it); a lap still open is not in the total until it is
// closed, so a line written from inside one cannot claim time it has not spent;
// the sentence turns from "took" into a statement that the window froze exactly
// AT the threshold and not a hair before it, and names the slowest lap when it
// does; and the log routes the two cases to two levels, because a report's
// warnings are what an engineer reads first.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/diag_log.hpp"
#include "core/phase_clock.hpp"
#include "test_check.hpp"

using cascade::core::DiagLog;
using cascade::core::PhaseClock;

namespace {

// A clock the test winds by hand.
struct Hand {
    double t = 1000.0;  // not zero: a lap measured from 0 would hide a missing subtraction
    PhaseClock::NowFn fn() {
        return [this] { return t; };
    }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

void testLapsAndTotal() {
    Hand h;
    PhaseClock c(h.fn());

    // Nothing run: no laps, no slowest, a total of nothing, and a sentence that
    // still reads - the early return of a function that did no work.
    CHECK(c.laps().empty());
    CHECK(c.slowest() == nullptr);
    CHECK(c.totalSeconds() == 0.0);
    CHECK(c.describe("plugins: reload", 5.0) == "plugins: reload took 0.0 s");

    c.begin("decoders");
    h.t += 2.0;
    c.begin("inventory");  // closes the first lap
    h.t += 0.5;
    c.end();

    CHECK(c.laps().size() == 2u);
    CHECK(c.laps()[0].name == "decoders");
    CHECK(c.laps()[0].seconds == 2.0);
    CHECK(c.laps()[1].name == "inventory");
    CHECK(c.laps()[1].seconds == 0.5);
    CHECK(c.totalSeconds() == 2.5);
    CHECK(c.slowest() != nullptr && c.slowest()->name == "decoders");
    std::printf("  %s\n", c.describe("plugins: reload", 5.0).c_str());
    CHECK(c.describe("plugins: reload", 5.0) ==
          "plugins: reload took 2.5 s (decoders 2.0, inventory 0.5)");

    // end() twice, and with nothing open, changes nothing.
    c.end();
    c.end();
    CHECK(c.laps().size() == 2u);
    CHECK(c.totalSeconds() == 2.5);
}

void testRepeatedNameAdds() {
    Hand h;
    PhaseClock c(h.fn());
    c.begin("load");
    h.t += 1.0;
    c.begin("unload");
    h.t += 1.0;
    c.begin("load");  // round again: the same lap, not a third
    h.t += 2.0;
    c.end();
    CHECK(c.laps().size() == 2u);
    CHECK(c.laps()[0].name == "load");
    CHECK(c.laps()[0].seconds == 3.0);
    CHECK(c.laps()[1].name == "unload");
    CHECK(c.laps()[1].seconds == 1.0);
    CHECK(c.totalSeconds() == 4.0);
}

void testOpenLapIsNotCounted() {
    Hand h;
    PhaseClock c(h.fn());
    c.begin("load");
    h.t += 4.0;
    // Four seconds are elapsing in a lap nobody has closed: a line written now
    // must not claim them.
    CHECK(c.totalSeconds() == 0.0);
    CHECK(c.laps().empty());
    c.end();
    CHECK(c.totalSeconds() == 4.0);
}

void testTheSentenceTurnsAtTheThreshold() {
    Hand h;
    {
        PhaseClock c(h.fn());
        c.begin("decoders");
        h.t += 2.0;
        c.begin("inventory");
        h.t += 116.0;
        c.begin("load");
        h.t += 0.4;
        c.end();
        const std::string s = c.describe("plugins: reload", 5.0);
        std::printf("  %s\n", s.c_str());
        CHECK(contains(s, "plugins: reload took 118.4 s"));
        CHECK(contains(s, "the window did not draw for that long"));
        // THE SENTENCE A REPORT IS READ FOR: which step.
        CHECK(contains(s, "slowest step: inventory 116.0 s"));
        // ...and the laps are still all there.
        CHECK(contains(s, "decoders 2.0"));
        CHECK(contains(s, "load 0.4"));
    }
    {
        // Just under, exactly at, and just over the threshold. A fresh hand for
        // each, and figures a binary double holds exactly, so "exactly at" is
        // exactly at and not a rounding error either side of it.
        Hand a;
        PhaseClock under(a.fn());
        under.begin("x");
        a.t += 4.75;
        under.end();
        CHECK(!contains(under.describe("w", 5.0), "did not draw"));

        Hand b;
        PhaseClock at(b.fn());
        at.begin("x");
        b.t += 5.0;
        at.end();
        CHECK(contains(at.describe("w", 5.0), "did not draw"));

        Hand c;
        PhaseClock over(c.fn());
        over.begin("x");
        c.t += 5.5;
        over.end();
        CHECK(contains(over.describe("w", 5.0), "did not draw"));
    }
    {
        // Nothing run is never a freeze, whatever the threshold says.
        PhaseClock none(h.fn());
        CHECK(!contains(none.describe("w", 5.0), "did not draw"));
    }
}

void testLogLevel() {
    Hand h;
    DiagLog& log = DiagLog::instance();

    log.resetForTest();
    PhaseClock fast(h.fn());
    fast.begin("load");
    h.t += 0.2;
    fast.end();
    fast.log("plugins: reload", 5.0);
    std::vector<std::string> ring = log.ringSnapshot();
    CHECK(ring.size() == 1u);
    if (ring.size() == 1u) {
        CHECK(contains(ring[0], " info plugins: reload took 0.2 s (load 0.2)"));
        CHECK(!contains(ring[0], " warn "));
    }

    log.resetForTest();
    PhaseClock slow(h.fn());
    slow.begin("unload");
    h.t += 120.0;
    slow.end();
    slow.log("plugins: reload", 5.0);
    ring = log.ringSnapshot();
    CHECK(ring.size() == 1u);
    if (ring.size() == 1u) {
        CHECK(contains(ring[0], " warn plugins: reload took 120.0 s"));
        CHECK(contains(ring[0], "slowest step: unload 120.0 s"));
    }
}

void testNullNames() {
    Hand h;
    PhaseClock c(h.fn());
    c.begin(nullptr);  // a caller's bug must not be the crash
    h.t += 1.0;
    c.end();
    CHECK(c.laps().size() == 1u);
    CHECK(c.laps()[0].name.empty());
    CHECK(contains(c.describe(nullptr, 5.0), "took 1.0 s"));
}

void testRealClockRuns() {
    // The default constructor measures with a real monotonic clock and gets a
    // non-negative answer: the one place the injected seam is not exercised.
    PhaseClock c;
    c.begin("x");
    c.end();
    CHECK(c.laps().size() == 1u);
    CHECK(c.laps()[0].seconds >= 0.0);
    CHECK(c.laps()[0].seconds < 5.0);
}

}  // namespace

int main() {
    std::printf("test_phase_clock\n");
    testLapsAndTotal();
    testRepeatedNameAdds();
    testOpenLapIsNotCounted();
    testTheSentenceTurnsAtTheThreshold();
    testLogLevel();
    testNullNames();
    testRealClockRuns();
    return testSummary("test_phase_clock");
}
