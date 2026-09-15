/*
 * THE DEMOD SCOPE'S ARITHMETIC: the trigger, the time base, the attenuator,
 * the graticule and the two reductions that turn samples into a beam.
 *
 * WHY EACH OF THESE IS HERE RATHER THAN BEING LOOKED AT ON SCREEN. A scope is
 * a measuring instrument, and every one of the numbers below is a claim it
 * makes about the signal - "this is five milliseconds", "that is twenty
 * millivolts", "this waveform is standing still". A picture cannot falsify any
 * of them. The two that would be hardest to spot by eye are the two the file
 * spends most of its length on:
 *
 *   THE HOLD-OFF. Without it the trigger lands on a different crossing of the
 *   same period each frame and the trace slides sideways, which looks exactly
 *   like a signal that is genuinely drifting. The check builds a waveform with
 *   a decoy crossing in it and proves the decoy is refused.
 *
 *   THE COLUMN REDUCTION. A trace drawn by taking one sample per pixel is an
 *   ALIAS, and an alias is a smooth, plausible, completely wrong waveform - the
 *   worst kind of display fault, because it looks like data. The check drives a
 *   tone fast enough to alias and requires the drawn envelope to still reach
 *   full amplitude in every column.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gui/demod_scope.hpp"
#include "test_check.hpp"

namespace {

using namespace cascade::gui;

constexpr double kPi = 3.14159265358979323846;

// --- the tube ----------------------------------------------------------------
void testGraticule() {
    // Ten by eight. The numbers themselves are the contract - a scope ruled
    // any other way is not the instrument the page claims to be, and every
    // readout ("ms/DIV", "mV/DIV") is a statement about these two.
    CHECK(kScopeDivX == 10);
    CHECK(kScopeDivY == 8);

    CHECK_NEAR(scopeGridLine(0, kScopeDivX, 100.0f, 300.0f), 100.0f, 1e-4);
    CHECK_NEAR(scopeGridLine(10, kScopeDivX, 100.0f, 300.0f), 300.0f, 1e-4);
    CHECK_NEAR(scopeGridLine(5, kScopeDivX, 100.0f, 300.0f), 200.0f, 1e-4);
    CHECK_NEAR(scopeGridLine(1, kScopeDivX, 0.0f, 100.0f), 10.0f, 1e-4);
    // Out of range CLAMPS rather than running off the bezel.
    CHECK_NEAR(scopeGridLine(-3, kScopeDivX, 100.0f, 300.0f), 100.0f, 1e-4);
    CHECK_NEAR(scopeGridLine(99, kScopeDivX, 100.0f, 300.0f), 300.0f, 1e-4);

    // EXACTLY ONE CENTRE LINE PER AXIS, and it is the axis: zero volts across
    // and mid-sweep down. The face draws it brighter, so "which line is zero"
    // has to be a fact and not a guess.
    int centresX = 0;
    for (int i = 0; i <= kScopeDivX; ++i) {
        if (scopeGridIsCentre(i, kScopeDivX)) { ++centresX; }
    }
    CHECK(centresX == 1);
    CHECK(scopeGridIsCentre(kScopeDivX / 2, kScopeDivX));
    int centresY = 0;
    for (int i = 0; i <= kScopeDivY; ++i) {
        if (scopeGridIsCentre(i, kScopeDivY)) { ++centresY; }
    }
    CHECK(centresY == 1);
    CHECK(scopeGridIsCentre(kScopeDivY / 2, kScopeDivY));
    // An odd ruling has no centre line at all, which is why the assertion
    // above is about this tube and not about arithmetic in general.
    CHECK(!scopeGridIsCentre(1, 3));
    CHECK(!scopeGridIsCentre(2, 3));
}

// --- the time base -----------------------------------------------------------
void testTimebase() {
    // A 1-2-5 ladder and nothing else: the sequence a bench instrument's
    // switch is detented in.
    CHECK(kScopeTimebaseCount == 6);
    const double want[] = {1.0, 2.0, 5.0, 10.0, 20.0, 50.0};
    for (int i = 0; i < kScopeTimebaseCount; ++i) {
        CHECK_NEAR(scopeTimebaseMs(i), want[i], 1e-9);
    }
    // A hand-edited config cannot open the scope on a step that is not there.
    CHECK(clampScopeTimebase(-4) == 0);
    CHECK(clampScopeTimebase(99) == kScopeTimebaseCount - 1);
    CHECK_NEAR(scopeTimebaseMs(-4), 1.0, 1e-9);
    CHECK_NEAR(scopeTimebaseMs(99), 50.0, 1e-9);

    // THE SWEEP IS TEN DIVISIONS, not one. Getting this wrong by a factor of
    // ten is the single easiest mistake in the whole page and it produces a
    // picture that looks entirely reasonable: 5 ms/DIV across a 48 kHz sink is
    // 2400 samples, and 240 would draw a tenth of the signal under a ruler
    // that says it is showing all of it.
    CHECK(scopeSweepSamples(5.0, 48000.0) == 2400);
    CHECK(scopeSweepSamples(1.0, 48000.0) == 480);
    CHECK(scopeSweepSamples(50.0, 48000.0) == 24000);
    // A stopped chain reports no rate, and no rate means no sweep.
    CHECK(scopeSweepSamples(5.0, 0.0) == 0);
    CHECK(scopeSweepSamples(5.0, -1.0) == 0);
    CHECK(scopeSweepSamples(0.0, 48000.0) == 0);
    // And the baseband tap runs at the CHANNEL rate, which is hundreds of
    // kilohertz: the cap is what stops the longest sweep asking for a hundred
    // million samples nobody could see.
    CHECK(scopeSweepSamples(50.0, 61.44e6) == 4u * 1024u * 1024u);
}

// --- the attenuator ----------------------------------------------------------
void testGainLadder() {
    CHECK(kScopeGainCount == 9);
    CHECK(clampScopeGain(-1) == 0);
    CHECK(clampScopeGain(1000) == kScopeGainCount - 1);
    // Strictly increasing, on the same 1-2-5 detents as the time base. A
    // ladder that repeated or went backwards would make the AUTO ranging
    // below stick.
    for (int i = 1; i < kScopeGainCount; ++i) {
        CHECK(scopeGainPerDiv(i) > scopeGainPerDiv(i - 1));
    }
    // Full scale is four divisions up, which is what makes the coarsest step
    // able to show a sample at digital full scale without clipping.
    CHECK_NEAR(scopeFullScale(kScopeGainCount - 1), 4.0f, 1e-6);
    CHECK_NEAR(scopeFullScale(0), 0.008f, 1e-6);
}

void testAutoGain() {
    // AN OVERLOADED TRACE WINDS THE ATTENUATOR BACK, one detent at a time.
    const int mid = 4;
    const float full = scopeFullScale(mid);
    CHECK(scopeAutoGain(full * 2.0f, mid) == mid + 1);
    // A SMALL ONE WINDS IT IN.
    CHECK(scopeAutoGain(full * 0.05f, mid) == mid - 1);
    // ANYWHERE COMFORTABLE, IT IS LEFT ALONE. This band is the whole design:
    // one threshold instead of two makes the attenuator hunt between adjacent
    // detents for ever, because the step that brings a signal just under full
    // scale puts it just over the "too small" line of the step below.
    CHECK(scopeAutoGain(full * 0.5f, mid) == mid);
    CHECK(scopeAutoGain(full * 0.9f, mid) == mid);
    CHECK(scopeAutoGain(full * 0.35f, mid) == mid);

    // AND IT ACTUALLY SETTLES. Range a fixed signal repeatedly from both ends
    // of the ladder and it must arrive somewhere and stay: a pair of
    // thresholds that overlapped would oscillate here for ever.
    const float signal = 0.3f;
    for (int start = 0; start < kScopeGainCount; ++start) {
        int g = start;
        for (int n = 0; n < 40; ++n) { g = scopeAutoGain(signal, g); }
        const int settled = g;
        for (int n = 0; n < 40; ++n) {
            g = scopeAutoGain(signal, g);
            CHECK(g == settled);
        }
        // And it settled somewhere the signal is actually visible: between a
        // third of the tube and the top of it.
        const float f = scopeFullScale(settled);
        CHECK(signal <= f * 0.95f);
        CHECK(signal >= f * 0.30f);
    }

    // SILENCE HOLDS THE STEP. A peak of zero is not a measurement of
    // amplitude, and winding to the most sensitive detent during a pause would
    // make the noise floor leap up the tube the moment the signal stopped.
    CHECK(scopeAutoGain(0.0f, 3) == 3);
    CHECK(scopeAutoGain(0.0f, 7) == 7);
    // The ends of the ladder are ends, not wrap-arounds.
    CHECK(scopeAutoGain(100.0f, kScopeGainCount - 1) == kScopeGainCount - 1);
    CHECK(scopeAutoGain(1.0e-9f, 0) == 0);
}

void testPeak() {
    const float x[] = {0.1f, -0.7f, 0.3f, -0.2f};
    CHECK_NEAR(scopePeak(x, 4), 0.7f, 1e-6);
    CHECK_NEAR(scopePeak(nullptr, 4), 0.0f, 1e-6);
    CHECK_NEAR(scopePeak(x, 0), 0.0f, 1e-6);
    // ONE NaN MUST NOT FREEZE THE ATTENUATOR. Comparing against a NaN is
    // false, so a peak that had adopted one would make every threshold in
    // scopeAutoGain false for ever after and the ranging would simply stop.
    const float withNan[] = {0.2f, std::nanf(""), 0.4f};
    CHECK_NEAR(scopePeak(withNan, 3), 0.4f, 1e-6);
}

// --- the trigger -------------------------------------------------------------

// A waveform with a DECOY in it: a long quiet stretch, a rise, a three-sample
// notch back below zero, another rise, then a long positive plateau. The notch
// produces a perfectly good rising zero crossing that a scope must refuse,
// because locking to it instead of to the one after the quiet stretch is what
// makes a trace slide sideways from frame to frame.
std::vector<float> decoyPeriod() {
    std::vector<float> p;
    p.insert(p.end(), 60, -1.0f);  // the quiet stretch (the real arming)
    p.insert(p.end(), 20, 1.0f);   // rise: the crossing worth having
    p.insert(p.end(), 3, -1.0f);   // the notch
    p.insert(p.end(), 40, 1.0f);   // rise again: the decoy's crossing
    p.insert(p.end(), 17, 1.0f);   // plateau, to make the period 140
    return p;
}

void testTriggerBasics() {
    // A plain rising crossing, found where it is.
    const float ramp[] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(scopeFindTrigger(ramp, 6, 1, 0.0f, 0.1f, 0) == 3);

    // NO EDGE AT ALL is the free-running case, and it has to be sayable.
    const float flat[] = {0.5f, 0.5f, 0.5f, 0.5f};
    CHECK(scopeFindTrigger(flat, 4, 1, 0.0f, 0.1f, 0) == kScopeNoTrigger);
    const float falling[] = {1.0f, 1.0f, -1.0f, -1.0f};
    CHECK(scopeFindTrigger(falling, 4, 1, 0.0f, 0.1f, 0) == kScopeNoTrigger);

    // Degenerate inputs answer, rather than reading off an end.
    CHECK(scopeFindTrigger(nullptr, 6, 1, 0.0f, 0.1f, 0) == kScopeNoTrigger);
    CHECK(scopeFindTrigger(ramp, 0, 1, 0.0f, 0.1f, 0) == kScopeNoTrigger);

    // ROOM FOR THE WHOLE SWEEP. A trigger with less than `need` samples after
    // it is useless: the sweep would run off the end of the buffer, and the
    // face would either draw a short trace or read past the end.
    CHECK(scopeFindTrigger(ramp, 6, 3, 0.0f, 0.1f, 0) == 3);
    CHECK(scopeFindTrigger(ramp, 6, 4, 0.0f, 0.1f, 0) == kScopeNoTrigger);

    // HYSTERESIS. A signal that rises out of the dead band without ever having
    // dipped below the lower threshold is noise around the level, not an edge.
    const float jitter[] = {-0.02f, -0.01f, -0.02f, 0.5f, 0.5f};
    CHECK(scopeFindTrigger(jitter, 5, 1, 0.0f, 0.1f, 0) == kScopeNoTrigger);
    // The same rise, once it has genuinely been below the threshold, is an edge.
    const float real[] = {-0.5f, -0.01f, -0.02f, 0.5f, 0.5f};
    CHECK(scopeFindTrigger(real, 5, 1, 0.0f, 0.1f, 0) == 3);
}

void testTriggerHoldoff() {
    // Start the buffer just before the notch, so the FIRST crossing available
    // is the decoy. With no hold-off the scope takes it; with a hold-off
    // longer than the notch it refuses and waits for the next real one.
    const std::vector<float> period = decoyPeriod();
    std::vector<float> buf;
    // Two periods, starting 5 samples before the notch at index 80.
    for (int r = 0; r < 3; ++r) { buf.insert(buf.end(), period.begin(), period.end()); }
    const std::size_t offset = 75;
    std::vector<float> shifted(buf.begin() + offset, buf.end());

    const std::size_t decoy = scopeFindTrigger(shifted.data(), shifted.size(), 100,
                                               0.0f, 0.1f, 0);
    CHECK(decoy != kScopeNoTrigger);
    // The notch sits at 80..82 of the period, i.e. 5..7 of `shifted`, so the
    // decoy crossing is at 8.
    CHECK(decoy == 8);

    const std::size_t honest = scopeFindTrigger(shifted.data(), shifted.size(), 100,
                                                0.0f, 0.1f, 20);
    CHECK(honest != kScopeNoTrigger);
    // The next crossing preceded by a genuine 60-sample quiet stretch: the
    // period is 140 long and its real crossing is at 60, so from an offset of
    // 75 that is 140 + 60 - 75 = 125.
    CHECK(honest == 125);
}

void testHoldoffAndHysteresisScale() {
    // BOTH OF THESE SCALE WITH SOMETHING THAT MOVES, and that is the whole
    // reason they are functions rather than constants.
    //
    // The hold-off is a quarter of ONE division, so it means the same thing at
    // every time base: at 1 ms/DIV it rejects ripples above a few kilohertz,
    // at 50 ms/DIV it rejects everything shorter than a syllable. A constant
    // number of samples would do one of those jobs and not the other.
    CHECK(scopeHoldoffSamples(scopeSweepSamples(1.0, 48000.0)) == 12);   // 0.25 ms
    CHECK(scopeHoldoffSamples(scopeSweepSamples(10.0, 48000.0)) == 120);  // 2.5 ms
    CHECK(scopeHoldoffSamples(scopeSweepSamples(50.0, 48000.0)) == 600);  // 12.5 ms
    // A quarter of a division, at every step on the ladder.
    for (int i = 0; i < kScopeTimebaseCount; ++i) {
        const std::size_t sweep = scopeSweepSamples(scopeTimebaseMs(i), 48000.0);
        CHECK(scopeHoldoffSamples(sweep) * 40u == sweep);
    }
    // Degenerate sweeps do not divide by anything.
    CHECK(scopeHoldoffSamples(0) == 0);

    // The hysteresis is a fraction of what FILLS THE TUBE, so the trigger
    // behaves the same at every gain. A fixed 20 mV dead band would be half
    // the screen at the most sensitive detent and invisible at the coarsest -
    // a trigger that behaves differently at different gains is one nobody can
    // predict.
    for (int i = 0; i < kScopeGainCount; ++i) {
        CHECK_NEAR(scopeHysteresis(i) / scopeFullScale(i), 0.02f, 1e-5);
    }
    CHECK(scopeHysteresis(kScopeGainCount - 1) > scopeHysteresis(0));
}

void testTriggerStandsStill() {
    // THE POINT OF ALL OF IT. Feed the same periodic waveform starting at
    // several different offsets - which is exactly what a rolling tap hands a
    // scope frame after frame - and the samples that follow the trigger must
    // be the SAME. That is what "the waveform stands still" means, and with a
    // hold-off of zero it is not true, because the trace locks to whichever
    // crossing happens to come first in that particular buffer.
    const std::vector<float> period = decoyPeriod();
    std::vector<float> buf;
    for (int r = 0; r < 6; ++r) { buf.insert(buf.end(), period.begin(), period.end()); }

    constexpr std::size_t kNeed = 140;
    std::vector<float> reference;
    bool haveReference = false;
    int slid = 0;
    for (std::size_t offset = 0; offset < 140; offset += 7) {
        std::vector<float> view(buf.begin() + static_cast<std::ptrdiff_t>(offset),
                                buf.end());
        const std::size_t t =
            scopeFindTrigger(view.data(), view.size(), kNeed, 0.0f, 0.1f, 20);
        CHECK(t != kScopeNoTrigger);
        if (t == kScopeNoTrigger) { continue; }
        std::vector<float> sweep(view.begin() + static_cast<std::ptrdiff_t>(t),
                                 view.begin() + static_cast<std::ptrdiff_t>(t + kNeed));
        if (!haveReference) {
            reference = sweep;
            haveReference = true;
            continue;
        }
        if (sweep != reference) { ++slid; }
    }
    CHECK(haveReference);
    CHECK(slid == 0);

    // AND THE SAME SWEEP WITHOUT HOLD-OFF DOES SLIDE, which is what makes the
    // check above a measurement rather than a tautology.
    int slidFree = 0;
    std::vector<float> freeRef;
    bool haveFree = false;
    for (std::size_t offset = 0; offset < 140; offset += 7) {
        std::vector<float> view(buf.begin() + static_cast<std::ptrdiff_t>(offset),
                                buf.end());
        const std::size_t t =
            scopeFindTrigger(view.data(), view.size(), kNeed, 0.0f, 0.1f, 0);
        if (t == kScopeNoTrigger) { continue; }
        std::vector<float> sweep(view.begin() + static_cast<std::ptrdiff_t>(t),
                                 view.begin() + static_cast<std::ptrdiff_t>(t + kNeed));
        if (!haveFree) {
            freeRef = sweep;
            haveFree = true;
            continue;
        }
        if (sweep != freeRef) { ++slidFree; }
    }
    CHECK(slidFree > 0);
}

// --- the trace ---------------------------------------------------------------
void testReduce() {
    // A TONE FAST ENOUGH TO ALIAS. 10 kHz at 48 kHz over a 50 ms/DIV sweep
    // puts about a hundred cycles in every column. Taking one sample per
    // column would draw a slow wandering line that is not in the signal at
    // all; the envelope of what fell in the column is the bright full-height
    // band a real tube shows.
    constexpr std::size_t kN = 24000;
    std::vector<float> x(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 10000.0 *
                                           static_cast<double>(i) / 48000.0));
    }
    constexpr std::size_t kCols = 900;
    std::vector<float> lo(kCols, 0.0f);
    std::vector<float> hi(kCols, 0.0f);
    CHECK(scopeReduce(x.data(), kN, kCols, lo.data(), hi.data()));
    int thin = 0;
    for (std::size_t c = 0; c < kCols; ++c) {
        if (hi[c] < 0.9f || lo[c] > -0.9f) { ++thin; }
        CHECK(lo[c] <= hi[c]);
    }
    CHECK(thin == 0);

    // The naive alternative, for the record: one sample per column, and it is
    // nowhere near full height. This is the fault the reduction prevents.
    int naiveThin = 0;
    for (std::size_t c = 0; c < kCols; ++c) {
        const float v = x[(kN * c) / kCols];
        if (v < 0.9f && v > -0.9f) { ++naiveThin; }
    }
    CHECK(naiveThin > static_cast<int>(kCols) / 2);

    // FEWER SAMPLES THAN COLUMNS still draws a line rather than gaps: every
    // column gets the nearest sample it has.
    const float few[] = {-1.0f, 0.0f, 1.0f};
    std::vector<float> lo2(10, 99.0f);
    std::vector<float> hi2(10, 99.0f);
    CHECK(scopeReduce(few, 3, 10, lo2.data(), hi2.data()));
    for (std::size_t c = 0; c < 10; ++c) {
        CHECK(lo2[c] >= -1.0f && lo2[c] <= 1.0f);
        CHECK(hi2[c] >= lo2[c]);
    }

    // Nothing in, nothing touched.
    CHECK(!scopeReduce(nullptr, 4, 4, lo2.data(), hi2.data()));
    CHECK(!scopeReduce(few, 0, 4, lo2.data(), hi2.data()));
    CHECK(!scopeReduce(few, 3, 0, lo2.data(), hi2.data()));
}

void testTraceY() {
    const float centre = 400.0f;
    const float divPx = 20.0f;  // 8 divisions = 160 px, so +/-80 from centre
    // Zero is the centre line, and up is up.
    CHECK_NEAR(scopeTraceY(0.0f, 0.1f, centre, divPx), centre, 1e-4);
    CHECK_NEAR(scopeTraceY(0.1f, 0.1f, centre, divPx), centre - 20.0f, 1e-4);
    CHECK_NEAR(scopeTraceY(-0.1f, 0.1f, centre, divPx), centre + 20.0f, 1e-4);
    // THE BEAM CLIPS AT THE PHOSPHOR. An unclamped trace would be drawn over
    // the bezel, the readouts and whatever page is behind the scope.
    CHECK_NEAR(scopeTraceY(10.0f, 0.1f, centre, divPx), centre - 80.0f, 1e-4);
    CHECK_NEAR(scopeTraceY(-10.0f, 0.1f, centre, divPx), centre + 80.0f, 1e-4);
    // A NaN sample lands on the axis rather than at an undefined pixel.
    CHECK_NEAR(scopeTraceY(std::nanf(""), 0.1f, centre, divPx), centre, 1e-4);
    // And a zero attenuator - which no ladder step is, but a corrupt config
    // could ask for - does not divide by it.
    CHECK_NEAR(scopeTraceY(0.5f, 0.0f, centre, divPx), centre, 1e-4);
}

// --- the spectrum mode -------------------------------------------------------
void testSpectrum() {
    // Twenty kilohertz, or the sink's Nyquist when that is lower. A scope must
    // never rule an axis over frequencies the chain cannot carry.
    CHECK_NEAR(scopeSpectrumSpanHz(48000.0), 20000.0, 1e-6);
    CHECK_NEAR(scopeSpectrumSpanHz(16000.0), 8000.0, 1e-6);
    CHECK_NEAR(scopeSpectrumSpanHz(44100.0), 20000.0, 1e-6);
    CHECK_NEAR(scopeSpectrumSpanHz(0.0), 0.0, 1e-6);

    // Decibels relative to full scale, with a floor well under the window so a
    // silent bin lands on the bottom rule instead of at negative infinity.
    CHECK_NEAR(scopeSpectrumDb(1.0f), 0.0f, 1e-4);
    CHECK_NEAR(scopeSpectrumDb(0.1f), -20.0f, 1e-3);
    CHECK_NEAR(scopeSpectrumDb(0.01f), -40.0f, 1e-3);
    CHECK(scopeSpectrumDb(0.0f) < kScopeSpectrumBottomDb);

    // Eighty decibels over the eight divisions is ten a division.
    CHECK_NEAR(kScopeSpectrumTopDb - kScopeSpectrumBottomDb, 80.0f, 1e-6);
    CHECK_NEAR(scopeSpectrumY(kScopeSpectrumTopDb, 100.0f, 500.0f), 100.0f, 1e-3);
    CHECK_NEAR(scopeSpectrumY(kScopeSpectrumBottomDb, 100.0f, 500.0f), 500.0f, 1e-3);
    CHECK_NEAR(scopeSpectrumY(-40.0f, 100.0f, 500.0f), 300.0f, 1e-3);
    // Clamped at both ends, for the reason the trace is.
    CHECK_NEAR(scopeSpectrumY(40.0f, 100.0f, 500.0f), 100.0f, 1e-3);
    CHECK_NEAR(scopeSpectrumY(-200.0f, 100.0f, 500.0f), 500.0f, 1e-3);

    // The bins inside the drawn span: 1024 bins of a 48 kHz transform are
    // 46.875 Hz each, so 20 kHz is bin 426 and the count is 427.
    CHECK(scopeSpectrumBins(1024, 48000.0, 20000.0) == 427);
    // And it never runs past the positive half of the transform, whatever the
    // span asks for.
    CHECK(scopeSpectrumBins(1024, 48000.0, 1.0e9) == 512);
    CHECK(scopeSpectrumBins(0, 48000.0, 20000.0) == 0);
    CHECK(scopeSpectrumBins(1024, 0.0, 20000.0) == 0);
}

// --- the vector mode ---------------------------------------------------------
void testVector() {
    float x = 0.0f;
    float y = 0.0f;
    const float cx = 500.0f;
    const float cy = 300.0f;
    const float divPx = 20.0f;

    scopeVectorPoint(0.0f, 0.0f, 0.1f, cx, cy, divPx, x, y);
    CHECK_NEAR(x, cx, 1e-4);
    CHECK_NEAR(y, cy, 1e-4);

    // THE SAME SCALE ON BOTH AXES, or the one thing this display exists to
    // show - the SHAPE an FM carrier or an SSB signal traces - is wrong. An
    // equal I and Q must land equally far out.
    scopeVectorPoint(0.1f, 0.1f, 0.1f, cx, cy, divPx, x, y);
    CHECK_NEAR(x - cx, 20.0f, 1e-4);
    CHECK_NEAR(cy - y, 20.0f, 1e-4);

    // Q is UP, the way every phasor diagram ever drawn has it.
    scopeVectorPoint(0.0f, 0.1f, 0.1f, cx, cy, divPx, x, y);
    CHECK(y < cy);

    // Clipped at the phosphor, both axes, both signs.
    scopeVectorPoint(9.0f, -9.0f, 0.1f, cx, cy, divPx, x, y);
    CHECK_NEAR(x, cx + 80.0f, 1e-4);
    CHECK_NEAR(y, cy + 80.0f, 1e-4);
    // NaN goes to the origin rather than off the page.
    scopeVectorPoint(std::nanf(""), std::nanf(""), 0.1f, cx, cy, divPx, x, y);
    CHECK_NEAR(x, cx, 1e-4);
    CHECK_NEAR(y, cy, 1e-4);
}

// --- the selector and the readouts -------------------------------------------
void testSignalSelector() {
    CHECK(kScopeSignalCount == 4);
    // A hand-edited config cannot open the scope on a signal that does not
    // exist, and every position has both a key word and a caption - a blank
    // key is a control nobody can find.
    CHECK(scopeSignalFromIndex(-1) == ScopeSignal::Audio);
    CHECK(scopeSignalFromIndex(0) == ScopeSignal::Audio);
    CHECK(scopeSignalFromIndex(3) == ScopeSignal::Vector);
    CHECK(scopeSignalFromIndex(99) == ScopeSignal::Vector);
    for (int i = 0; i < kScopeSignalCount; ++i) {
        const ScopeSignal s = scopeSignalFromIndex(i);
        CHECK(scopeSignalKey(s)[0] != '\0');
        CHECK(scopeSignalCaption(s)[0] != '\0');
    }
    // WHICH TAP EACH POSITION NEEDS. The page reads one of two rings and asks
    // this question to decide; getting it wrong shows the audio trace under
    // the caption VECTOR I-Q.
    CHECK(!scopeSignalIsBaseband(ScopeSignal::Audio));
    CHECK(!scopeSignalIsBaseband(ScopeSignal::Spectrum));
    CHECK(scopeSignalIsBaseband(ScopeSignal::Baseband));
    CHECK(scopeSignalIsBaseband(ScopeSignal::Vector));
}

// --- is anything arriving? ---------------------------------------------------
void testLiveness() {
    // THE CHECK THIS REPLACED, AND WHY IT WAS WRONG, is worth reproducing
    // because it looked completely reasonable: "did the tap's counter change
    // since the last frame". This application renders at about 1400 frames a
    // second, and a DSP block of audio arrives every half millisecond or so,
    // so on most frames the answer is legitimately NO - and the scope drew
    // "NO SAMPLES - RECEIVER STOPPED" across a receiver that was plainly
    // running. Whether samples are ARRIVING is a question about a window of
    // time, and asking it at a single instant gets the wrong answer.
    //
    // Frames at 1400 a second against a producer that arrives in BURSTS every
    // two milliseconds - which is what a DSP thread that drains its ring and
    // then sleeps actually looks like, and is what the real counters did. Two
    // frames in three see no change at all, and every one of them must still
    // read as live.
    ScopeLiveness st;
    std::uint64_t written = 1000;
    double now = 0.0;
    CHECK(scopeTapLive(st, written, now));
    int deadFrames = 0;
    int noChangeFrames = 0;
    for (int f = 1; f <= 1400; ++f) {
        now = static_cast<double>(f) / 1400.0;
        const std::uint64_t before = written;
        written = 1000 + 96u * static_cast<std::uint64_t>(now / 0.002);
        if (written == before) { ++noChangeFrames; }
        if (!scopeTapLive(st, written, now)) { ++deadFrames; }
    }
    // The fixture really does contain the case that broke it...
    CHECK(noChangeFrames > 100);
    // ...and not one of those frames reads as a stopped receiver.
    CHECK(deadFrames == 0);

    // A STOPPED RECEIVER IS STILL SAID, and promptly. The counter freezes;
    // within a quarter of a second the tube says so.
    const double stoppedAt = now;
    bool wentDead = false;
    double deadAt = 0.0;
    for (int f = 1; f <= 2000; ++f) {
        now = stoppedAt + static_cast<double>(f) / 1400.0;
        if (!scopeTapLive(st, written, now)) {
            wentDead = true;
            deadAt = now;
            break;
        }
    }
    CHECK(wentDead);
    CHECK(deadAt - stoppedAt >= kScopeStaleSeconds);
    CHECK(deadAt - stoppedAt < kScopeStaleSeconds + 0.01);

    // And it comes back the instant the counter moves again.
    written += 24;
    CHECK(scopeTapLive(st, written, now + 0.001));

    // A TAP THAT HAS NEVER BEEN WRITTEN IS NEVER LIVE, whatever the clock
    // says. "Nothing has ever arrived" and "it has gone quiet" are different
    // statements; only the second one is about a signal.
    ScopeLiveness fresh;
    CHECK(!scopeTapLive(fresh, 0, 0.0));
    CHECK(!scopeTapLive(fresh, 0, 0.001));
    CHECK(scopeTapLive(fresh, 5, 0.002));
}

void testFormatting() {
    char buf[32];
    formatScopeTimebase(buf, sizeof(buf), 5.0);
    CHECK(std::strcmp(buf, "5 ms/DIV") == 0);
    formatScopeTimebase(buf, sizeof(buf), 50.0);
    CHECK(std::strcmp(buf, "50 ms/DIV") == 0);
    formatScopeTimebase(buf, sizeof(buf), 0.5);
    CHECK(std::strcmp(buf, "500 us/DIV") == 0);

    formatScopeGain(buf, sizeof(buf), 0.02f);
    CHECK(std::strcmp(buf, "20 mV/DIV") == 0);
    formatScopeGain(buf, sizeof(buf), 1.0f);
    CHECK(std::strcmp(buf, "1 V/DIV") == 0);
}

}  // namespace

int main() {
    testGraticule();
    testTimebase();
    testGainLadder();
    testAutoGain();
    testPeak();
    testTriggerBasics();
    testTriggerHoldoff();
    testHoldoffAndHysteresisScale();
    testTriggerStandsStill();
    testReduce();
    testTraceY();
    testSpectrum();
    testVector();
    testSignalSelector();
    testLiveness();
    testFormatting();
    return testSummary("test_demod_scope");
}
