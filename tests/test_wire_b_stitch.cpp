// test_wire_b_stitch.cpp - THE STITCHED SWEEP'S STATE MACHINE (engine/wire_b_stitch.{hpp,cpp}; wire-b second pass, item 10), with no engine and no radio: the caller's time and what the radio shows are
// handed in, so every branch is driven by hand.
//
//   P1  the plan: centres cover both ends and are never further apart than the step, one centre for a narrow range, over 512 centres refused, every setting refused just outside its range
//   P2  a full pass: four retunes, ONE line, the levels of captures taken with different gains aligned to the first
//   P3  frames of one centre are averaged as POWER (-60 dB and -70 dB read -62.6 dB, not -65)
//   P4  failures: a radio that does not follow, a changed sample rate, a centre moved in the middle, a bin count that changes, no frames, frames that repeat; the caller's own fail()
//   P5  passes: 2 gives two lines then Finished, 0 goes on, cancel ends it
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/wire_b_stitch.hpp"
#include "test_check.hpp"
#ifndef CHECK_EQ
#define CHECK_EQ(a, b) CHECK((a) == (b))
#endif

using namespace cascade::engine::wireb;

namespace {

constexpr double kRate = 240000.0;
constexpr std::size_t kBins = 1024;

// A radio the test drives: it follows a retune when `follow`, and gives a new frame at every call when `frames`.
struct Radio {
    double centre = 100.0e6;
    double rate = kRate;
    bool follow = true;
    bool frames = true;
    bool newSeq = true;
    std::uint64_t seq = 1;
    std::size_t bins = kBins;
    // the level of the frame for the current centre: (centre - 100 MHz) / 144 kHz steps of -5 dB, plus `extra` on alternate frames
    double levelFor(double c) const { return -80.0 - 5.0 * std::round((c - 100.0912e6) / 139.2e3) * (gain ? 1.0 : 0.0); }
    bool gain = true;
    double extra = 0.0;
    std::vector<float> db;
    StitchObservation obs() {
        if (frames && newSeq) { ++seq; }
        db.assign(bins, static_cast<float>(levelFor(centre) + ((seq % 2) != 0 ? extra : 0.0)));
        StitchObservation o;
        o.haveFrame = frames;
        o.seq = seq;
        o.db = &db;
        o.centreHz = centre;
        o.rateHz = rate;
        return o;
    }
};

struct Counts {
    int retunes = 0, lines = 0, finished = 0, failed = 0;
    cascade::core::SweepLine last;
};

// Runs the machine against `radio` for at most `maxSteps` calls 10 ms apart. Stops at Finished or Failed.
Counts drive(StitchRun& run, Radio& radio, int maxSteps, double t0 = 100.0) {
    Counts c;
    double t = t0;
    for (int i = 0; i < maxSteps; ++i, t += 0.01) {
        const StitchStep st = run.advance(t, radio.obs());
        if (st == StitchStep::Retune) { ++c.retunes; if (radio.follow) { radio.centre = run.retuneTo(); } }
        else if (st == StitchStep::LineReady) { ++c.lines; run.takeLine(c.last); }
        else if (st == StitchStep::Finished) { ++c.finished; break; }
        else if (st == StitchStep::Failed) { ++c.failed; break; }
    }
    return c;
}

StitchSettings range(double a, double b) {
    StitchSettings s;
    s.startHz = a;
    s.stopHz = b;
    s.settleMs = 50;
    s.frames = 2;
    return s;
}

}  // namespace

int main() {
    std::string why;

    // ------------------------------------------------------------------ P1 the plan
    std::printf("P1: the plan\n");
    {
        StitchRun run;
        CHECK(run.begin(range(100.0e6, 100.6e6), kRate, 0.0, why));
        const std::vector<double>& c = run.centres();
        CHECK_EQ(c.size(), static_cast<std::size_t>(4));
        const double half = 0.38 * kRate;
        CHECK(std::fabs(c.front() - half - 100.0e6) < 1.0);   // the lower edge of the first capture's kept bins is the start
        CHECK(std::fabs(c.back() + half - 100.6e6) < 1.0);    // and the upper edge of the last is the stop
        for (std::size_t i = 1; i < c.size(); ++i) { CHECK(c[i] - c[i - 1] <= 0.6 * kRate + 1.0); CHECK(c[i] > c[i - 1]); }
        StitchRun narrow;
        CHECK(narrow.begin(range(100.0e6, 100.1e6), kRate, 0.0, why));
        CHECK_EQ(narrow.centres().size(), static_cast<std::size_t>(1));
        CHECK(std::fabs(narrow.centres()[0] - 100.05e6) < 1.0);
        StitchRun big;
        CHECK(!big.begin(range(1.0e6, 6.0e9), kRate, 0.0, why));
        CHECK(why.find("512") != std::string::npos);
        CHECK(!big.active());
        const auto bad = [&](StitchSettings s, double rate = kRate) { StitchRun r; return !r.begin(s, rate, 0.0, why) && !why.empty(); };
        CHECK(bad(range(100.6e6, 100.0e6)));
        CHECK(bad(range(100.0e6, 100.0e6)));
        CHECK(bad(range(-1.0, 100.0e6)));
        CHECK(bad(range(100.0e6, 100.6e6), 0.0));   // a radio with no sample rate
        StitchSettings s = range(100.0e6, 100.6e6);
        s.stepFraction = 0.29; CHECK(bad(s)); s.stepFraction = 0.71; CHECK(bad(s));
        s.stepFraction = 0.3; { StitchRun r; CHECK(r.begin(s, kRate, 0.0, why)); }
        s.stepFraction = 0.7; { StitchRun r; CHECK(r.begin(s, kRate, 0.0, why)); }
        s = range(100.0e6, 100.6e6); s.trimFraction = 0.41; CHECK(bad(s)); s.trimFraction = -0.01; CHECK(bad(s));
        s.trimFraction = 0.0; { StitchRun r; CHECK(r.begin(s, kRate, 0.0, why)); }
        s = range(100.0e6, 100.6e6); s.settleMs = -1; CHECK(bad(s)); s.settleMs = 5001; CHECK(bad(s));
        s = range(100.0e6, 100.6e6); s.frames = 0; CHECK(bad(s)); s.frames = 65; CHECK(bad(s));
        s = range(100.0e6, 100.6e6); s.timeoutMs = 499; CHECK(bad(s)); s.timeoutMs = 60001; CHECK(bad(s));
        s = range(100.0e6, 100.6e6); s.passes = -1; CHECK(bad(s)); s.passes = 1000001; CHECK(bad(s));
    }

    // ------------------------------------------------------------------ P2 a full pass
    std::printf("P2: one pass\n");
    {
        StitchRun run;
        Radio radio;
        CHECK(run.begin(range(100.0e6, 100.6e6), kRate, 0.0, why));
        CHECK(run.active());
        const Counts c = drive(run, radio, 2000);
        std::printf("  %d retunes, %d line(s), %d finished, %d failed\n", c.retunes, c.lines, c.finished, c.failed);
        CHECK_EQ(c.retunes, 4);
        CHECK_EQ(c.lines, 1);
        CHECK_EQ(c.finished, 1);
        CHECK_EQ(c.failed, 0);
        CHECK(!run.active());
        CHECK_EQ(run.passesDone(), static_cast<std::uint64_t>(1));
        CHECK_EQ(run.seams(), static_cast<std::size_t>(3));
        CHECK_EQ(run.alignedSeams(), static_cast<std::size_t>(3));
        CHECK(c.last.db.size() > 2000);
        // every capture was seen 5 dB lower than the one before: after the alignment the line is flat at the first capture's level
        double lo = 1e9, hi = -1e9;
        for (const float v : c.last.db) { if (std::isfinite(v)) { lo = std::min(lo, static_cast<double>(v)); hi = std::max(hi, static_cast<double>(v)); } }
        std::printf("  line %.2f to %.2f dB\n", lo, hi);
        CHECK(lo > -81.0 && hi < -79.0);
        CHECK(c.last.startHz <= 100.01e6 && c.last.stopHz >= 100.59e6);
        CHECK(run.advance(500.0, radio.obs()) == StitchStep::Nothing);   // nothing after the end
        cascade::core::SweepLine none;
        CHECK(!run.takeLine(none));   // the line was taken once
    }

    // ------------------------------------------------------------------ P3 power averaging
    std::printf("P3: frames are averaged as power\n");
    {
        StitchSettings s = range(100.0e6, 100.1e6);
        StitchRun run;
        Radio radio;
        radio.gain = false;
        radio.extra = -10.0;   // frames alternate -80 and -90 ... see below: the base level is -80 for gain false
        radio.centre = 100.05e6;
        // base -80 (frame 0, 2, ...) and -90 (odd frames): the two frames of one centre are -80 and -90 -> power mean of 1e-8 and 1e-9 = 5.5e-9 = -82.6 dB
        CHECK(run.begin(s, kRate, 0.0, why));
        const Counts c = drive(run, radio, 2000);
        CHECK_EQ(c.lines, 1);
        double sum = 0.0;
        std::size_t n = 0;
        for (const float v : c.last.db) { if (std::isfinite(v)) { sum += v; ++n; } }
        const double mean = n != 0 ? sum / static_cast<double>(n) : 0.0;
        std::printf("  mean level %.2f dB (power mean -82.6, dB mean -85.0)\n", mean);
        CHECK(std::fabs(mean + 82.6) < 0.2);
    }

    // ------------------------------------------------------------------ P4 failures
    std::printf("P4: failures\n");
    {
        const auto fails = [&](Radio& radio, StitchSettings s, const char* word) {
            StitchRun run;
            CHECK(run.begin(s, kRate, 0.0, why));
            const Counts c = drive(run, radio, 3000);
            std::printf("  %s\n", run.failure().c_str());
            CHECK_EQ(c.failed, 1);
            CHECK_EQ(c.lines, 0);
            CHECK(run.failure().find(word) != std::string::npos);
            CHECK(!run.active());
            CHECK(run.advance(900.0, radio.obs()) == StitchStep::Nothing);
        };
        StitchSettings s = range(100.0e6, 100.6e6);
        s.timeoutMs = 500;
        { Radio r; r.follow = false; fails(r, s, "did not retune"); }
        { Radio r; r.frames = false; fails(r, s, "no spectrum frames"); }
        { Radio r; r.newSeq = false; fails(r, s, "no spectrum frames"); }   // the same frame over and over is not a stream
        {   // the sample rate changes after the second retune
            StitchRun run;
            Radio r;
            CHECK(run.begin(s, kRate, 0.0, why));
            double t = 100.0;
            int retunes = 0;
            for (int i = 0; i < 3000 && run.active(); ++i, t += 0.01) {
                if (retunes == 2) { r.rate = 2.0 * kRate; }
                const StitchStep st = run.advance(t, r.obs());
                if (st == StitchStep::Retune) { ++retunes; r.centre = run.retuneTo(); }
            }
            CHECK(run.failure().find("sample rate") != std::string::npos);
        }
        {   // the user moves the radio's centre while it collects
            StitchRun run;
            Radio r;
            CHECK(run.begin(s, kRate, 0.0, why));
            double t = 100.0;
            bool moved = false;
            for (int i = 0; i < 3000 && run.active(); ++i, t += 0.01) {
                const StitchStep st = run.advance(t, r.obs());
                if (st == StitchStep::Retune) { r.centre = run.retuneTo(); }
                if (!moved && run.collecting()) { r.centre += 0.2 * kRate; moved = true; }   // (the next frame shows it)
            }
            CHECK(run.failure().find("centre was changed") != std::string::npos);
        }
        {   // the bin count changes between two frames of one centre
            StitchRun run;
            Radio r;
            StitchSettings s2 = range(100.0e6, 100.1e6);
            s2.frames = 3;
            CHECK(run.begin(s2, kRate, 0.0, why));
            double t = 100.0;
            int inCollect = 0;
            for (int i = 0; i < 3000 && run.active(); ++i, t += 0.01) {
                const StitchStep st = run.advance(t, r.obs());
                if (st == StitchStep::Retune) { r.centre = run.retuneTo(); }
                if (run.collecting() && ++inCollect == 2) { r.bins = 512; }   // after one frame was taken
            }
            CHECK(run.failure().find("size changed") != std::string::npos);
        }
        {   // the caller could not retune
            StitchRun run;
            Radio r;
            CHECK(run.begin(s, kRate, 0.0, why));
            CHECK(run.advance(100.0, r.obs()) == StitchStep::Retune);
            run.fail("the radio refused the retune: locked");
            CHECK(!run.active());
            CHECK(run.failure().find("locked") != std::string::npos);
            CHECK(run.advance(100.1, r.obs()) == StitchStep::Nothing);
        }
    }

    // ------------------------------------------------------------------ P5 passes and cancel
    std::printf("P5: passes\n");
    {
        StitchSettings s = range(100.0e6, 100.6e6);
        s.passes = 2;
        StitchRun run;
        Radio radio;
        CHECK(run.begin(s, kRate, 0.0, why));
        const Counts c = drive(run, radio, 5000);
        CHECK_EQ(c.lines, 2);
        CHECK_EQ(c.retunes, 8);
        CHECK_EQ(c.finished, 1);
        CHECK_EQ(run.passesDone(), static_cast<std::uint64_t>(2));
        s.passes = 0;
        StitchRun forever;
        Radio radio2;
        CHECK(forever.begin(s, kRate, 0.0, why));
        const Counts d = drive(forever, radio2, 3000);
        std::printf("  passes 0: %d lines in 3000 calls, still active %d\n", d.lines, forever.active() ? 1 : 0);
        CHECK(d.lines >= 3);
        CHECK_EQ(d.finished, 0);
        CHECK(forever.active());
        forever.cancel();
        CHECK(!forever.active());
        CHECK(forever.advance(900.0, radio2.obs()) == StitchStep::Nothing);
        CHECK(forever.failure().empty());   // a close is not a failure
    }
    return testSummary("test_wire_b_stitch");
}
