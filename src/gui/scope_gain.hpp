// scope_gain.hpp - THE RADAR SCOPE'S GAIN KNOB, as decisions rather than as drawing (window port group 12,
// engine-stage4.md 6.5; API 0.3 section 8.11).
//
// The knob on the scope's plinth drives the FIRST gain stage the open radio lists. What it shows (its travel, the
// five figures lettered round it and the one lit, the readout, where the pointer sits) and what a detent does (two
// units, clamped to the travel) are arithmetic over four numbers the engine's table carries - the stage's minimum,
// its maximum, its readback and its unit - and none of it needs a GL context or a frame. It is here so that
// tests/test_scope_gain.cpp can hold every rule by itself and tests/test_scope_window.cpp can drive the knob through
// the real window; the window (AppWindow::drawScopeMode) draws the knob and sends the command, and the numbers are
// these.
//
// THE LOCAL COPY. A command the knob sends lands at the engine's next pass, and the stage's readback (GAINS value[3],
// which the lists' fingerprint hashes) shows it a pass after that. A knob that computed each detent from the READBACK
// would, with two detents in two frames before the pass had published, add both to the same old figure and lose one.
// So the knob keeps the figure it sent (ScopeGainLocal) and computes the next from that, until the readback has
// caught up with it (equal: the engine took exactly that), or the stage is no longer the one it was sent to, or a
// moment has gone by (the engine took something else - a radio that quantises, a refusal - and the readback is the
// truth). The same shape as the VFO drag's tuneLocal_, with a release that does not depend on how many frames a pass
// takes: that one is dropped when the next state is read, which is right while the frame IS the pass and loses a
// detent the day the pass runs on its own thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SCOPE_GAIN_HPP
#define CASCADE_GUI_SCOPE_GAIN_HPP

#include <algorithm>
#include <cmath>
#include <string>

namespace cascade::gui {

// A detent is two units of the stage, in the stage's own unit: decibels on every radio but the Airspy R2/Mini, whose
// stages are register steps (two steps a detent crosses its 0..14 LNA in seven).
inline constexpr float kScopeGainDetentUnits = 2.0f;
// How many ticks are lettered round the knob.
inline constexpr int kScopeKnobTickCount = 5;
// How long a sent figure is kept as the base of the next detent when the readback does not come to equal it.
inline constexpr double kScopeGainLocalHoldS = 0.5;

// The knob's travel: the stage's own range when it reports one (an RTL-SDR's first stage runs 0..33.5, a B200's PGA to
// 76), the fallback when a driver reports none (a range with no width).
struct ScopeKnobTravel {
    float loDb = 0.0f;
    float hiDb = 0.0f;
};
inline ScopeKnobTravel scopeKnobTravel(float stageLo, float stageHi, float fallbackLo, float fallbackHi) {
    if (stageHi > stageLo) { return {stageLo, stageHi}; }
    return {fallbackLo, fallbackHi};
}

// The five figures lettered along the arc, in whole units, evenly across the travel (the last is the maximum).
inline void scopeKnobTicks(float lo, float hi, int out[kScopeKnobTickCount]) {
    for (int i = 0; i < kScopeKnobTickCount; ++i) {
        out[i] = static_cast<int>(lo + (hi - lo) * static_cast<float>(i) / 4.0f);
    }
}

// The tick nearest the figure, 0..4; -1 when there is none to light (the knob is dead, or the travel has no width).
inline int scopeKnobLitTick(float db, float lo, float hi, bool live) {
    if (!live) { return -1; }
    const float span = hi - lo;
    if (!(span > 0.0f)) { return -1; }
    const int sel = static_cast<int>((db - lo) / span * 4.0f + 0.5f);
    return std::clamp(sel, 0, kScopeKnobTickCount - 1);
}

// Where the figure sits on the travel, for the pointer: 0.5 when there is no radio or no width to put it on. (Not
// clamped: the knob's own drawing clamps the pointer.)
inline float scopeKnobFraction(bool haveGain, float db, float lo, float hi) {
    if (haveGain && hi > lo) { return (db - lo) / (hi - lo); }
    return 0.5f;
}

// What `steps` detents (negative anticlockwise) make of `baseDb`: two units each, held to the travel.
inline float scopeKnobNextDb(float baseDb, int steps, float lo, float hi) {
    return std::clamp(baseDb + static_cast<float>(steps) * kScopeGainDetentUnits, lo, hi);
}

// THE FIGURE A SENT COMMAND WILL HAVE MADE, kept until the engine's readback shows it.
struct ScopeGainLocal {
    bool valid = false;
    std::string stage;     // the stage it was sent to (a different first stage - another radio - is not it)
    float db = 0.0f;       // the figure sent
    double sentAtS = 0.0;  // when (ImGui's clock)
};

// Does the kept figure still stand? Not when it is empty, the stage has changed, the readback has come to equal it
// (the engine caught up: the readback is the figure from here), or the hold has run out.
inline bool scopeGainLocalStands(const ScopeGainLocal& l, const std::string& stage, float readbackDb, double nowS) {
    return l.valid && l.stage == stage && readbackDb != l.db && (nowS - l.sentAtS) < kScopeGainLocalHoldS;
}

// The figure the knob shows and takes its next detent from.
inline float scopeGainShown(const ScopeGainLocal& l, const std::string& stage, float readbackDb, double nowS) {
    return scopeGainLocalStands(l, stage, readbackDb, nowS) ? l.db : readbackDb;
}

// WHAT THE KNOB LAST LETTERED, kept by the window every frame the cabinet is drawn: the figures are drawn into the
// draw list (not widget text, so no log of a frame holds them) and a test reads them here.
struct ScopeKnobView {
    bool drawn = false;     // the cabinet drew the knob in the last frame it was asked for
    bool haveGain = false;  // a radio is open and lists a stage
    bool live = false;      // the knob answers the hand (a radio, without its own AGC)
    float loDb = 0.0f;      // the travel
    float hiDb = 0.0f;
    float shownDb = 0.0f;   // the figure it shows and takes its next detent from
    float fraction = 0.5f;  // where the pointer sits on the travel
    int lit = -1;           // the tick lit; -1 none
    int ticks[kScopeKnobTickCount] = {};
    std::string readout;    // "30 dB", "7", "AUTO", "N/A"
    std::string note;       // the stage's name, "auto gain is on" or "no device"
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SCOPE_GAIN_HPP
