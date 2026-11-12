// test_ppm_auto.cpp - THE PILOT PPM MEASUREMENT AS A MODEL (engine serial lane A, row R18): core/ppm_auto.hpp.
//
//   A  a pilot 0.38 Hz low (19 kHz seen from a crystal 20 ppm fast) reads +20 ppm; 0.38 Hz high reads -20;
//   B  the first two seconds, unlocked readings and weak pilots are not counted; fewer than 20 counted readings = Failed (no pilot), 20 or more = Done;
//   C  the uncertainty is the standard error of the mean: tenfold the readings, a third of the spread;
//   D  seconds are held to 10..300; a new start forgets the old readings; cancel returns to idle; a reading after the end changes nothing;
//   E  non-finite readings are not counted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>

#include "core/ppm_auto.hpp"
#include "test_check.hpp"

using cascade::core::ppmauto::Measurement;
using cascade::core::ppmauto::Result;
using cascade::core::ppmauto::State;

namespace {
// Feeds readings 0.1 s apart from t=0 with the given offset and a wobble of +-wobbleHz alternating, until the time is up.
Result run(Measurement& m, double seconds, double offsetHz, double wobbleHz, bool locked = true, double level = 1.0) {
    m.start(seconds, 0.0);
    double t = 0.0;
    int i = 0;
    while (t <= m.seconds() + 0.05) {
        m.add(t, locked, level, offsetHz + ((i++ & 1) != 0 ? wobbleHz : -wobbleHz));
        t += 0.1;
    }
    return m.result(t);
}
}  // namespace

int main() {
    std::printf("test_ppm_auto\n");
    {   // A
        Measurement m;
        Result r = run(m, 20.0, -0.38, 0.0);
        std::printf("  A: pilot -0.38 Hz -> %+.2f ppm (state %d, %d readings)\n", r.ppm, static_cast<int>(r.state), r.readings);
        CHECK(r.state == State::Done);
        CHECK(std::fabs(r.ppm - 20.0) < 1e-6);
        r = run(m, 20.0, +0.38, 0.0);
        CHECK(r.state == State::Done && std::fabs(r.ppm + 20.0) < 1e-6);
        r = run(m, 20.0, 0.0, 0.0);
        CHECK(r.state == State::Done && std::fabs(r.ppm) < 1e-9);
    }
    {   // B
        Measurement m;
        Result r = run(m, 20.0, -0.38, 0.0);
        CHECK(r.readings >= 175 && r.readings <= 185);                 // 20 s at 10 a second less the 2 s warm-up
        r = run(m, 20.0, -0.38, 0.0, false);
        CHECK(r.state == State::Failed && r.readings == 0);            // never locked: nothing counted, Failed
        r = run(m, 20.0, -0.38, 0.0, true, 0.3);
        CHECK(r.state == State::Failed && r.readings == 0);            // pilot too weak
        // 19 counted readings fail, 20 pass
        Measurement few;
        few.start(10.0, 0.0);
        for (int i = 0; i < 19; ++i) { few.add(2.0 + 0.1 * i, true, 1.0, -0.38); }
        few.add(10.0, false, 1.0, 0.0);
        CHECK(few.result(10.0).state == State::Failed && few.result(10.0).readings == 19);
        Measurement twenty;
        twenty.start(10.0, 0.0);
        for (int i = 0; i < 20; ++i) { twenty.add(2.0 + 0.1 * i, true, 1.0, -0.38); }
        twenty.add(10.0, false, 1.0, 0.0);
        CHECK(twenty.result(10.0).state == State::Done);
        // readings in the first two seconds are not counted
        Measurement warm;
        warm.start(10.0, 0.0);
        for (int i = 0; i < 19; ++i) { warm.add(0.1 * i, true, 1.0, -0.38); }   // t = 0 .. 1.8 s
        CHECK(warm.result(1.8).readings == 0);
    }
    {   // C
        Measurement m;
        const Result a = run(m, 10.0, -0.2, 0.05);
        const Result b = run(m, 100.0, -0.2, 0.05);
        std::printf("  C: %d readings +-%.3f ppm, %d readings +-%.3f ppm\n", a.readings, a.uncertaintyPpm, b.readings, b.uncertaintyPpm);
        CHECK(a.uncertaintyPpm > 0.0 && b.uncertaintyPpm > 0.0);
        CHECK(b.uncertaintyPpm < 0.45 * a.uncertaintyPpm);
        CHECK(std::fabs(a.ppm - 10.526) < 0.1 && std::fabs(b.ppm - 10.526) < 0.1);
        // a wobble of +-0.05 Hz is a standard deviation of 0.05 Hz = 2.63 ppm; the error of the mean is that over sqrt(n)
        CHECK(std::fabs(b.uncertaintyPpm - 2.63 / std::sqrt(static_cast<double>(b.readings))) < 0.03);
    }
    {   // D
        Measurement m;
        m.start(1.0, 0.0);
        CHECK(m.seconds() == 10.0);
        m.start(9999.0, 0.0);
        CHECK(m.seconds() == 300.0);
        Result r = run(m, 20.0, -0.38, 0.0);
        CHECK(r.state == State::Done);
        m.start(20.0, 0.0);
        CHECK(m.active() && m.result(0.0).readings == 0 && m.result(0.0).state == State::Measuring);
        m.cancel();
        CHECK(!m.active() && m.result(0.0).state == State::Idle);
        const Result done = run(m, 20.0, -0.38, 0.0);
        m.add(100.0, true, 1.0, 5.0);                                   // after the end: nothing
        CHECK(m.result(100.0).readings == done.readings && m.result(100.0).ppm == done.ppm);
    }
    {   // E
        Measurement m;
        m.start(10.0, 0.0);
        m.add(3.0, true, 1.0, std::nan(""));
        m.add(4.0, true, 1.0, INFINITY);
        CHECK(m.result(4.0).readings == 0);
    }
    return testSummary("test_ppm_auto");
}
