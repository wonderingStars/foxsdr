// test_scope_gain.cpp - THE RADAR SCOPE'S GAIN KNOB AS ARITHMETIC (window port group 12, gui/scope_gain.hpp).
//
// What the knob shows and what a detent does are decisions over four numbers the engine's table carries (the first
// gain stage's minimum, maximum and readback, and its unit) and one the window keeps (the figure its last detent sent).
// They are held here with no window, no frame and no engine; tests/test_scope_window.cpp drives the same rules through
// the real window.
//
//   A  the travel: the stage's own range, the fallback for a stage that reports none
//   B  the five figures lettered round the knob, and the tick that is lit
//   C  the pointer's fraction
//   D  what a detent does: two units, either way, held to the travel (the edges, a step count of several, none)
//   E  the local copy: it stands until the readback equals it, the stage changes, or the hold runs out; two detents
//      before the engine has shown the first are two detents
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "gui/scope_gain.hpp"
#include "test_check.hpp"

namespace g = cascade::gui;

static void testTravel() {
    std::printf("  A: the travel\n");
    // A real stage: its own range.
    const g::ScopeKnobTravel rtl = g::scopeKnobTravel(0.0f, 49.6f, 0.0f, 60.0f);
    CHECK(rtl.loDb == 0.0f && rtl.hiDb == 49.6f);
    const g::ScopeKnobTravel b200 = g::scopeKnobTravel(0.0f, 76.0f, 0.0f, 60.0f);
    CHECK(b200.hiDb == 76.0f);
    // An attenuator-style stage whose range does not start at zero.
    const g::ScopeKnobTravel neg = g::scopeKnobTravel(-10.0f, 20.0f, 0.0f, 60.0f);
    CHECK(neg.loDb == -10.0f && neg.hiDb == 20.0f);
    // No range (min == max, or inverted, or nothing listed): the fallback.
    CHECK(g::scopeKnobTravel(5.0f, 5.0f, 0.0f, 60.0f).hiDb == 60.0f);
    CHECK(g::scopeKnobTravel(9.0f, 3.0f, 1.0f, 60.0f).loDb == 1.0f);
    CHECK(g::scopeKnobTravel(0.0f, 0.0f, 0.0f, 60.0f).hiDb == 60.0f);
}

static void testTicks() {
    std::printf("  B: the ticks and the lit one\n");
    int t[g::kScopeKnobTickCount];
    g::scopeKnobTicks(0.0f, 49.6f, t);
    // 0, 12.4, 24.8, 37.2, 49.6 as whole units (cut, not rounded: the old expression).
    CHECK(t[0] == 0 && t[1] == 12 && t[2] == 24 && t[3] == 37 && t[4] == 49);
    g::scopeKnobTicks(0.0f, 76.0f, t);
    CHECK(t[0] == 0 && t[1] == 19 && t[2] == 38 && t[3] == 57 && t[4] == 76);
    g::scopeKnobTicks(0.0f, 14.0f, t);  // an Airspy's LNA, in register steps
    CHECK(t[0] == 0 && t[2] == 7 && t[4] == 14);

    // The tick nearest the figure.
    CHECK(g::scopeKnobLitTick(0.0f, 0.0f, 49.6f, true) == 0);
    CHECK(g::scopeKnobLitTick(29.7f, 0.0f, 49.6f, true) == 2);   // 29.7 / 49.6 * 4 = 2.39 -> 2
    CHECK(g::scopeKnobLitTick(31.5f, 0.0f, 49.6f, true) == 3);   // 2.54 -> 3 (nearest, not cut)
    CHECK(g::scopeKnobLitTick(49.6f, 0.0f, 49.6f, true) == 4);
    // Past either end (a readback outside the stage's own range): the nearest end, not an index off the scale.
    CHECK(g::scopeKnobLitTick(80.0f, 0.0f, 49.6f, true) == 4);
    CHECK(g::scopeKnobLitTick(-30.0f, 0.0f, 49.6f, true) == 0);
    // Dead (AGC, no radio): nothing lit. A travel with no width: nothing lit.
    CHECK(g::scopeKnobLitTick(29.7f, 0.0f, 49.6f, false) == -1);
    CHECK(g::scopeKnobLitTick(10.0f, 5.0f, 5.0f, true) == -1);
}

static void testFraction() {
    std::printf("  C: the pointer\n");
    CHECK(g::scopeKnobFraction(true, 0.0f, 0.0f, 50.0f) == 0.0f);
    CHECK(g::scopeKnobFraction(true, 25.0f, 0.0f, 50.0f) == 0.5f);
    CHECK(g::scopeKnobFraction(true, 50.0f, 0.0f, 50.0f) == 1.0f);
    CHECK(g::scopeKnobFraction(true, -10.0f, 0.0f, 50.0f) < 0.0f);  // (not clamped here: the drawing clamps the pointer)
    // No radio, or no width: the middle.
    CHECK(g::scopeKnobFraction(false, 10.0f, 0.0f, 50.0f) == 0.5f);
    CHECK(g::scopeKnobFraction(true, 10.0f, 5.0f, 5.0f) == 0.5f);
}

static void testNext() {
    std::printf("  D: a detent\n");
    CHECK(g::kScopeGainDetentUnits == 2.0f);
    CHECK(g::scopeKnobNextDb(30.0f, 1, 0.0f, 49.6f) == 32.0f);
    CHECK(g::scopeKnobNextDb(30.0f, -1, 0.0f, 49.6f) == 28.0f);
    CHECK(g::scopeKnobNextDb(30.0f, 3, 0.0f, 49.6f) == 36.0f);    // several detents in one frame
    CHECK(g::scopeKnobNextDb(30.0f, -2, 0.0f, 49.6f) == 26.0f);
    CHECK(g::scopeKnobNextDb(30.0f, 0, 0.0f, 49.6f) == 30.0f);
    // Held to the travel, either end; a figure already at the end stays there (no command is due: next == base).
    CHECK(g::scopeKnobNextDb(48.5f, 1, 0.0f, 49.6f) == 49.6f);
    CHECK(g::scopeKnobNextDb(49.6f, 1, 0.0f, 49.6f) == 49.6f);
    CHECK(g::scopeKnobNextDb(1.0f, -1, 0.0f, 49.6f) == 0.0f);
    CHECK(g::scopeKnobNextDb(0.0f, -5, 0.0f, 49.6f) == 0.0f);
    CHECK(g::scopeKnobNextDb(10.0f, 100, 0.0f, 49.6f) == 49.6f);
    // A stage in steps (an Airspy's 0..14) crosses its travel in seven detents.
    float v = 0.0f;
    for (int i = 0; i < 7; ++i) { v = g::scopeKnobNextDb(v, 1, 0.0f, 14.0f); }
    CHECK(v == 14.0f);
    // A readback past the travel is brought back into it by the next detent (the clamp is on the result).
    CHECK(g::scopeKnobNextDb(60.0f, -1, 0.0f, 49.6f) == 49.6f);
}

static void testLocalCopy() {
    std::printf("  E: the local copy\n");
    g::ScopeGainLocal l;
    // Empty: never stands, and shows the readback.
    CHECK(!g::scopeGainLocalStands(l, "TUNER", 30.0f, 10.0));
    CHECK(g::scopeGainShown(l, "TUNER", 30.0f, 10.0) == 30.0f);

    // A detent sent at t = 10 s with the readback at 30: the figure sent stands over the old readback...
    l.valid = true;
    l.stage = "TUNER";
    l.db = 32.0f;
    l.sentAtS = 10.0;
    CHECK(g::scopeGainLocalStands(l, "TUNER", 30.0f, 10.0));
    CHECK(g::scopeGainShown(l, "TUNER", 30.0f, 10.0) == 32.0f);
    CHECK(g::scopeGainShown(l, "TUNER", 30.0f, 10.2) == 32.0f);
    // ...not once the readback equals it (the engine caught up: the readback is the figure from here)...
    CHECK(!g::scopeGainLocalStands(l, "TUNER", 32.0f, 10.0));
    CHECK(g::scopeGainShown(l, "TUNER", 32.0f, 10.0) == 32.0f);
    // ...nor for another stage (a different radio)...
    CHECK(!g::scopeGainLocalStands(l, "PGA", 30.0f, 10.0));
    CHECK(g::scopeGainShown(l, "PGA", 30.0f, 10.0) == 30.0f);
    // ...nor once the hold has run out (the engine took something else: the readback is the truth). The edge: held
    // for kScopeGainLocalHoldS exclusive.
    CHECK(g::scopeGainLocalStands(l, "TUNER", 31.9f, 10.0 + g::kScopeGainLocalHoldS - 0.001));
    CHECK(!g::scopeGainLocalStands(l, "TUNER", 31.9f, 10.0 + g::kScopeGainLocalHoldS));
    CHECK(g::scopeGainShown(l, "TUNER", 31.9f, 10.0 + g::kScopeGainLocalHoldS + 1.0) == 31.9f);

    // TWO DETENTS BEFORE THE ENGINE HAS SHOWN THE FIRST: the second is taken from the figure the first sent.
    const float lo = 0.0f, hi = 49.6f, readback = 30.0f;
    g::ScopeGainLocal k;
    float shown = g::scopeGainShown(k, "TUNER", readback, 1.0);
    float sent1 = g::scopeKnobNextDb(shown, 1, lo, hi);
    k = {true, "TUNER", sent1, 1.0};
    shown = g::scopeGainShown(k, "TUNER", readback, 1.0 + 1.0 / 60.0);  // the readback has not moved
    float sent2 = g::scopeKnobNextDb(shown, 1, lo, hi);
    CHECK(sent1 == 32.0f && sent2 == 34.0f);
    // Without the copy the second would be 32 again: one detent lost.
    CHECK(g::scopeKnobNextDb(readback, 1, lo, hi) == sent1);
    // The first lands (readback 32) while the second is in flight: the second's figure still stands.
    k = {true, "TUNER", sent2, 1.0 + 1.0 / 60.0};
    CHECK(g::scopeGainShown(k, "TUNER", 32.0f, 1.05) == 34.0f);
    // ...and when it lands too, the readback is the figure.
    CHECK(g::scopeGainShown(k, "TUNER", 34.0f, 1.06) == 34.0f);
    CHECK(!g::scopeGainLocalStands(k, "TUNER", 34.0f, 1.06));
    // A detent against the end: no command is due, so the figure does not change and nothing is kept.
    CHECK(g::scopeKnobNextDb(hi, 1, lo, hi) == hi);
}

int main() {
    std::printf("test_scope_gain\n");
    testTravel();
    testTicks();
    testFraction();
    testNext();
    testLocalCopy();
    return testSummary("test_scope_gain");
}
