// The DEMOD SCOPE's pure logic (src/slint/scope_logic.hpp) and its pictures (scope_compositor.hpp) against a SCRIPTED TAP READER: the gather (triggered sweeps, the spectrum, the I/Q
// split, the capacity clamp, liveness), the AUTO ranging, the keys and their refusals, the readouts and the dead tube's sentences, the multiplex's marks, and the painter's output
// bytes. No Slint, no engine, no window. The arithmetic itself (trigger, ladders, reductions, memory) is pinned by tests/test_demod_scope.cpp over the same headers.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_compositor.hpp"
#include "scope_logic.hpp"
#include "gui/fm_mpx_plan.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slintscope;
using foxsdr::slinthost::Rgba;
namespace gui = cascade::gui;

namespace {

constexpr double kPi = 3.14159265358979323846;

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A script for the three taps: a tone on the audio tap, a circle on the I/Q tap, a pilot on the multiplex tap, each with a `written` counter the test moves.
struct ScriptTaps : ScopeTapReader {
    double audioRate = 48000.0, iqRate = 200000.0;
    float audioAmp = 0.5f, audioHz = 1000.0f, iqAmp = 0.3f, iqHz = 5000.0f;
    std::uint64_t writtenAudio = 1000, writtenIq = 1000, writtenMpx = 1000;
    std::uint32_t capAudio = 65536, capIq = 262144, capMpx = 65536;
    bool failAudio = false;
    bool starve = false;                   // data reads return no frames (the info still answers)
    std::vector<std::uint32_t> asked;      // the capFrames of every data read, with its tap in the high byte
    int infoReads = 0;
    std::uint64_t phase = 0;               // a running sample counter so a later read continues the tone

    std::int32_t readScope(std::uint32_t tap, FoxScopeInfo* info, float* out, std::uint32_t capFrames) override {
        if (tap == FOXAPI_SCOPE_AUDIO && failAudio) { return -1; }
        if (info != nullptr) {
            ++infoReads;
            info->tap = tap;
            info->channels = tap == FOXAPI_SCOPE_IQ ? 2 : 1;
            info->capacityFrames = tap == FOXAPI_SCOPE_AUDIO ? capAudio : tap == FOXAPI_SCOPE_IQ ? capIq : capMpx;
            info->rateHz = tap == FOXAPI_SCOPE_AUDIO ? audioRate : iqRate;
            info->written = tap == FOXAPI_SCOPE_AUDIO ? writtenAudio : tap == FOXAPI_SCOPE_IQ ? writtenIq : writtenMpx;
        }
        if (out == nullptr || capFrames == 0 || starve) { return 0; }
        asked.push_back(capFrames | (tap << 24));
        const std::uint32_t cap = tap == FOXAPI_SCOPE_AUDIO ? capAudio : tap == FOXAPI_SCOPE_IQ ? capIq : capMpx;
        const std::uint32_t n = std::min(capFrames, cap);
        for (std::uint32_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) + 123.0;   // an arbitrary start, so the trigger has something to find
            if (tap == FOXAPI_SCOPE_AUDIO) {
                out[i] = audioAmp * static_cast<float>(std::sin(2.0 * kPi * audioHz * t / audioRate));
            } else if (tap == FOXAPI_SCOPE_IQ) {
                out[2 * i] = iqAmp * static_cast<float>(std::cos(2.0 * kPi * iqHz * t / iqRate));
                out[2 * i + 1] = iqAmp * static_cast<float>(std::sin(2.0 * kPi * iqHz * t / iqRate));
            } else {
                out[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 19000.0 * t / iqRate));
            }
        }
        return static_cast<std::int32_t>(n);
    }
};

DemodInputs inputs(double nowS, bool wfm = false) {
    DemodInputs in;
    in.wfm = wfm;
    in.audioRateHz = 48000.0;
    in.channelRateHz = 200000.0;
    in.nowS = nowS;
    return in;
}

// a first look and a second one a moment later with the counters moved: the tap is live from the second tick (the first only records the counter)
void warm(DemodScope& s, ScriptTaps& t, double& now, bool wfm = false) {
    s.tick(t, inputs(now, wfm));
    now += 0.033;
    t.writtenAudio += 1600;
    t.writtenIq += 6600;
    t.writtenMpx += 6600;
    s.tick(t, inputs(now, wfm));
}

std::size_t countWhere(const std::vector<Rgba>& px, bool (*pred)(const Rgba&)) {
    return static_cast<std::size_t>(std::count_if(px.begin(), px.end(), pred));
}

void spectrumTransform() {
    // a full-scale tone exactly on a bin reads about 0 dB there and far below everywhere else
    std::vector<float> tone(kFftAudio);
    const std::size_t bin = 64;
    for (std::size_t i = 0; i < tone.size(); ++i) { tone[i] = static_cast<float>(std::sin(2.0 * kPi * static_cast<double>(bin) * static_cast<double>(i) / static_cast<double>(kFftAudio))); }
    std::vector<float> db;
    fftPowerDb(tone.data(), tone.size(), 800, db);
    CHECK(db.size() == 800);
    if (db.size() == 800) {
        CHECK_NEAR(db[bin], 0.0, 1.0);
        float worst = -1000.0f;
        for (std::size_t k = 0; k < db.size(); ++k) {
            if (k + 3 < bin || k > bin + 3) { worst = std::max(worst, db[k]); }
        }
        CHECK(worst < -60.0f);
    }
    // a size that is not a power of two, null input and zero bins answer nothing rather than reading past the buffer
    fftPowerDb(tone.data(), 1000, 100, db);
    CHECK(db.empty());
    fftPowerDb(nullptr, kFftAudio, 100, db);
    CHECK(db.empty());
    fftPowerDb(tone.data(), kFftAudio, 0, db);
    CHECK(db.empty());
    // more bins than the positive half are clamped to it
    fftPowerDb(tone.data(), kFftAudio, 100000, db);
    CHECK(db.size() == kFftAudio / 2);
    // the multiplex's longer transform puts a 19 kHz pilot of a 200 kHz channel in the right bin
    std::vector<float> mpx(kFftMpx);
    for (std::size_t i = 0; i < mpx.size(); ++i) { mpx[i] = static_cast<float>(std::sin(2.0 * kPi * 19000.0 * static_cast<double>(i) / 200000.0)); }
    fftPowerDb(mpx.data(), mpx.size(), 4000, db);
    std::size_t peak = 0;
    for (std::size_t k = 0; k < db.size(); ++k) { if (db[k] > db[peak]) { peak = k; } }
    CHECK(std::abs(static_cast<double>(peak) * 200000.0 / static_cast<double>(kFftMpx) - 19000.0) < 25.0);
}

void gatherAudio() {
    ScriptTaps taps;
    taps.audioHz = 100.0f;   // the classic's hold-off wants a quarter division below the hysteresis band: a 1 kHz tone at 10 ms/DIV never arms the trigger and runs free
    DemodScope s;
    double now = 10.0;
    s.state.autoGain = false;
    warm(s, taps, now);
    const DemodFeed& f = s.feed();
    CHECK(s.live());
    CHECK(f.live);
    // 10 ms/DIV x 10 divisions at 48 kHz
    CHECK(f.audioCount == 4800);
    CHECK_NEAR(f.audioRateHz, 48000.0, 0.5);
    // TRIGGERED: the sweep starts at a rising zero crossing (the hysteresis lets it start a little after it), not at the arbitrary phase the tap began at
    if (f.audio != nullptr && f.audioCount > 8) {
        CHECK(f.audio[0] < 0.25f * taps.audioAmp);
        CHECK(f.audio[8] > f.audio[0]);
    }
    // the search room: two sweeps asked for, one sweep drawn
    bool askedTwo = false;
    for (const std::uint32_t a : taps.asked) { if ((a >> 24) == FOXAPI_SCOPE_AUDIO && (a & 0xFFFFFFu) == 9600u) { askedTwo = true; } }
    CHECK(askedTwo);
    // no I/Q and no spectrum on the audio position
    CHECK(f.iqI == nullptr && f.iqCount == 0 && f.spectrumDb == nullptr);
    // THE CAPACITY CLAMP: a ring smaller than two sweeps is asked for what it holds, and the sweep stays inside it
    ScriptTaps small;
    small.capAudio = 6000;
    DemodScope s2;
    s2.state.autoGain = false;
    double now2 = 5.0;
    warm(s2, small, now2);
    bool clamped = false;
    for (const std::uint32_t a : small.asked) { if ((a >> 24) == FOXAPI_SCOPE_AUDIO && (a & 0xFFFFFFu) == 6000u) { clamped = true; } }
    CHECK(clamped);
    CHECK(s2.feed().audioCount == 4800);
    // a ring that holds less than one sweep shortens the sweep to what it holds (a time base that quietly stops meaning what it says is the classic's cap too)
    ScriptTaps tiny;
    tiny.capAudio = 1000;
    DemodScope s3;
    s3.state.autoGain = false;
    double now3 = 5.0;
    warm(s3, tiny, now3);
    CHECK(s3.feed().audioCount == 1000);
    // a tap that is counting but gives no frames leaves the tube with nothing in it, and the sentence says so
    tiny = ScriptTaps{};
    tiny.starve = true;
    DemodScope s5;
    s5.state.autoGain = false;
    double now5 = 5.0;
    warm(s5, tiny, now5);
    CHECK(s5.live());
    CHECK(s5.feed().audio == nullptr);
    CHECK(s5.readouts().dead == cascade::i18n::tr("NO AUDIO IN THE TAP"));
    // the time base changes the sweep: 1 ms/DIV is 480 samples
    DemodScope s4;
    s4.state.autoGain = false;
    s4.state.timebase = 0;
    double now4 = 5.0;
    ScriptTaps t4;
    warm(s4, t4, now4);
    CHECK(s4.feed().audioCount == 480);
}

void liveness() {
    ScriptTaps taps;
    DemodScope s;
    double now = 1.0;
    s.state.autoGain = false;
    // the very first look only records the counter, but a counter above zero is live
    s.tick(taps, inputs(now));
    CHECK(s.live());
    // the counter stands still for longer than a quarter second: the tube is dead and says why, and the feed is empty (no flat line at zero)
    now += 0.3;
    s.tick(taps, inputs(now));
    CHECK(!s.live());
    CHECK(s.feed().audio == nullptr);
    CHECK(s.readouts().dead == cascade::i18n::tr("NO SAMPLES - RECEIVER STOPPED"));
    // it moves again: live again
    now += 0.033;
    taps.writtenAudio += 1600;
    s.tick(taps, inputs(now));
    CHECK(s.live());
    CHECK(s.readouts().dead.empty());
    // a counter of zero is never live
    ScriptTaps zero;
    zero.writtenAudio = 0;
    DemodScope s2;
    s2.tick(zero, inputs(2.0));
    CHECK(!s2.live());
    // a failed read counts as a counter that did not move: the tube goes dead after the stale time, not at once
    ScriptTaps bad;
    DemodScope s3;
    double n3 = 1.0;
    warm(s3, bad, n3);
    CHECK(s3.live());
    bad.failAudio = true;
    n3 += 0.1;
    s3.tick(bad, inputs(n3));
    CHECK(s3.live());
    n3 += 0.3;
    s3.tick(bad, inputs(n3));
    CHECK(!s3.live());
    // the tap's own rate stands in for nothing when it says 0: the state's figure is used
    ScriptTaps norate;
    norate.audioRate = 0.0;
    DemodScope s4;
    s4.state.autoGain = false;
    double n4 = 1.0;
    warm(s4, norate, n4);
    CHECK_NEAR(s4.feed().audioRateHz, 48000.0, 0.5);
}

void spectrumAndMultiplex() {
    ScriptTaps taps;
    DemodScope s;
    s.state.signal = 1;   // SPEC
    double now = 3.0;
    warm(s, taps, now);
    const DemodFeed& f = s.feed();
    CHECK(f.spectrumDb != nullptr);
    CHECK(f.spectrumBins > 100);
    CHECK_NEAR(f.spectrumBinHz, 48000.0 / 2048.0, 1e-6);
    CHECK(f.audio == nullptr);
    if (f.spectrumDb != nullptr && f.spectrumBins > 100) {
        std::size_t peak = 0;
        for (std::size_t k = 0; k < f.spectrumBins; ++k) { if (f.spectrumDb[k] > f.spectrumDb[peak]) { peak = k; } }
        CHECK(std::abs(static_cast<double>(peak) * f.spectrumBinHz - 1000.0) < 30.0);
        CHECK(f.spectrumDb[peak] > -12.0f);   // a half-scale tone is about -6 dB
    }
    // the axis is what the bins cover: (bins-1) * binHz, at most the 20 kHz span
    CHECK_NEAR(f.spectrumSpanHz, static_cast<double>(f.spectrumBins - 1) * f.spectrumBinHz, 1e-6);
    CHECK(f.spectrumSpanHz <= 20000.0 + f.spectrumBinHz);
    const DemodReadouts r = s.readouts();
    CHECK(r.spectral);
    CHECK(r.gain == "10 dB/DIV");
    CHECK(r.centre.rfind("0 - ", 0) == 0);
    CHECK(r.timebaseKey == cascade::i18n::tr("-- (spectrum)"));
    // the multiplex: absent outside WFM (a saved MPX is shown as SPEC meanwhile and NOT rewritten), present in WFM with its marks
    DemodScope m;
    m.state.signal = static_cast<int>(gui::ScopeSignal::Mpx);
    double nm = 3.0;
    ScriptTaps tm;
    warm(m, tm, nm, false);
    CHECK(m.signalNow() == gui::ScopeSignal::Spectrum);
    CHECK(m.state.signal == static_cast<int>(gui::ScopeSignal::Mpx));
    CHECK(!m.mpxAvailable());
    CHECK(m.mpxMarks().empty());
    nm += 0.033;
    tm.writtenMpx += 6600;
    tm.writtenAudio += 1600;
    m.tick(tm, inputs(nm, true));
    CHECK(m.signalNow() == gui::ScopeSignal::Mpx);
    CHECK(m.mpxAvailable());
    CHECK(m.feed().spectrumBins > 1000);
    CHECK_NEAR(m.feed().spectrumBinHz, 200000.0 / static_cast<double>(kFftMpx), 1e-6);
    const std::vector<MpxMark> marks = m.mpxMarks();
    CHECK(marks.size() == gui::kMpxBandCount);   // all seven fit a 100 kHz span
    int tones = 0;
    for (const MpxMark& mk : marks) {
        CHECK(mk.x0 >= 0.0 && mk.x1 <= 1.0 + 1e-9 && mk.x1 >= mk.x0);
        if (mk.tone) { ++tones; CHECK(mk.x0 == mk.x1); }
    }
    CHECK(tones == 3);   // the pilot and the two SCAs
    CHECK(m.readouts().caption == cascade::i18n::tr("FM MULTIPLEX"));
    // the key refuses MPX outside WFM and accepts it inside
    DemodScope k;
    k.tick(tm, inputs(1.0, false));
    CHECK(!k.setSignal(static_cast<int>(gui::ScopeSignal::Mpx)));
    k.tick(tm, inputs(1.1, true));
    CHECK(k.setSignal(static_cast<int>(gui::ScopeSignal::Mpx)));
}

void baseband() {
    ScriptTaps taps;
    DemodScope s;
    s.state.signal = 2;   // I/Q
    s.state.autoGain = false;
    double now = 4.0;
    warm(s, taps, now);
    const DemodFeed& f = s.feed();
    // 10 ms/DIV at 200 kHz: 20 000 frames, 40 000 asked
    CHECK(f.iqCount == 20000);
    CHECK(f.iqI != nullptr && f.iqQ != nullptr && f.audio == nullptr);
    bool asked = false;
    for (const std::uint32_t a : taps.asked) { if ((a >> 24) == FOXAPI_SCOPE_IQ && (a & 0xFFFFFFu) == 40000u) { asked = true; } }
    CHECK(asked);
    // DE-INTERLEAVED WITH BOTH ARMS SHIFTED TOGETHER: I and Q stay a quarter turn apart (a circle), so I^2 + Q^2 is the amplitude squared at every sample
    if (f.iqI != nullptr && f.iqQ != nullptr) {
        double worst = 0.0;
        for (std::size_t i = 0; i < f.iqCount; i += 97) {
            worst = std::max(worst, std::abs(static_cast<double>(f.iqI[i]) * f.iqI[i] + static_cast<double>(f.iqQ[i]) * f.iqQ[i] - 0.09));
        }
        CHECK(worst < 1e-4);
    }
    CHECK(s.readouts().caption == cascade::i18n::tr("BASEBAND I/Q"));
    // the vector display reads the same arrays
    s.setSignal(3);
    now += 0.033;
    taps.writtenIq += 6600;
    s.tick(taps, inputs(now));
    CHECK(s.feed().iqCount == 20000);
    // a dead I/Q tap is its own sentence, and a tap that stalls leaves the audio one alone
    ScriptTaps empty;
    DemodScope e;
    e.state.signal = 3;
    double ne = 4.0;
    warm(e, empty, ne);
    empty.starve = true;
    ne += 0.033;
    empty.writtenIq += 6600;
    e.tick(empty, inputs(ne));
    CHECK(e.feed().iqI == nullptr);
    CHECK(e.readouts().dead == cascade::i18n::tr("NO BASEBAND IN THE TAP"));
}

void autoRanging() {
    // a half-scale tone on 100 mV/DIV (full scale 0.4 V): over 95 % of full scale, so the attenuator steps to the next detent each tick, ONE step a tick, and holds once it fits
    ScriptTaps taps;
    DemodScope s;
    CHECK(s.state.autoGain);
    CHECK(s.state.gainIndex == 5);
    double now = 2.0;
    s.tick(taps, inputs(now));
    CHECK(s.state.gainIndex == 6);
    now += 0.033;
    taps.writtenAudio += 1600;
    s.tick(taps, inputs(now));
    CHECK(s.state.gainIndex == 6);   // 0.5 against 0.8 full scale: between 30 % and 95 %: hold
    // a tone that falls well under the scale steps back in
    taps.audioAmp = 0.05f;
    now += 0.033;
    taps.writtenAudio += 1600;
    s.tick(taps, inputs(now));
    CHECK(s.state.gainIndex == 5);
    // a silent feed (peak 0) holds the step rather than winding it all the way in
    taps.audioAmp = 0.0f;
    for (int i = 0; i < 5; ++i) {
        now += 0.033;
        taps.writtenAudio += 1600;
        s.tick(taps, inputs(now));
    }
    CHECK(s.state.gainIndex == 5);
    // the ladder's ends hold: never past 1 V/DIV
    DemodScope top;
    top.state.gainIndex = 8;
    ScriptTaps loud;
    loud.audioAmp = 9.0f;
    double nt = 1.0;
    top.tick(loud, inputs(nt));
    CHECK(top.state.gainIndex == 8);
    // AUTO off: the figure is not touched by the ranging
    DemodScope hand;
    hand.state.autoGain = false;
    double nh = 1.0;
    ScriptTaps t2;
    hand.tick(t2, inputs(nh));
    CHECK(hand.state.gainIndex == 5);
}

void keys() {
    DemodScope s;
    ScriptTaps taps;
    s.tick(taps, inputs(1.0));
    // the time base steps and stops at its ends (the ladder 1, 2, 5, 10, 20, 50 ms)
    CHECK(s.state.timebase == 3);
    CHECK(s.stepTimebase(+1));
    CHECK(s.state.timebase == 4);
    CHECK(s.stepTimebase(+1));
    CHECK(s.state.timebase == 5);
    CHECK(!s.stepTimebase(+1));
    CHECK(s.state.timebase == 5);
    for (int i = 0; i < 9; ++i) { s.stepTimebase(-1); }
    CHECK(s.state.timebase == 0);
    CHECK(!s.stepTimebase(-1));
    // the attenuator stepper is refused while AUTO is ticked, works after it is turned off, and stops at its ends
    CHECK(!s.stepGain(+1));
    CHECK(s.setAutoGain(false));
    CHECK(!s.setAutoGain(false));
    const int g0 = s.state.gainIndex;
    CHECK(s.stepGain(+1));
    CHECK(s.state.gainIndex == g0 + 1);
    for (int i = 0; i < 20; ++i) { s.stepGain(+1); }
    CHECK(s.state.gainIndex == 8);
    CHECK(!s.stepGain(+1));
    for (int i = 0; i < 20; ++i) { s.stepGain(-1); }
    CHECK(s.state.gainIndex == 0);
    CHECK(!s.stepGain(-1));
    // the display keys
    CHECK(s.setDisplay(2));
    CHECK(!s.setDisplay(2));
    CHECK(s.state.display == 2);
    CHECK(s.setDisplay(99));    // out of the vocabulary is NORM (the settings file's clamp), a change from PERSIST
    CHECK(s.state.display == 0);
    CHECK(!s.setDisplay(-5));   // and again: no change
    CHECK(s.state.display == 0);
    // the input keys
    CHECK(s.setSignal(1));
    CHECK(!s.setSignal(1));
    // on a spectrum both steppers are disabled whatever the AUTO latch says
    CHECK(!s.stepTimebase(+1));
    CHECK(!s.stepGain(+1));
    CHECK(s.setSignal(0));
    CHECK(s.setSignal(2));
    CHECK(s.setSignal(3));
    // an index outside the vocabulary is the vector position (the classic's clamp), not a crash
    CHECK(!s.setSignal(77));   // already VECTOR
    // every change marks the picture dirty; painting clears it
    s.markPainted();
    CHECK(!s.dirty());
    s.setSignal(0);
    CHECK(s.dirty());
}

void readoutWords() {
    ScriptTaps taps;
    DemodScope s;
    s.state.autoGain = false;
    double now = 1.0;
    DemodInputs in = inputs(now);
    in.audioFrom = "dab-radio";
    s.tick(taps, in);
    now += 0.033;
    taps.writtenAudio += 1600;
    in.nowS = now;
    s.tick(taps, in);
    DemodReadouts r = s.readouts();
    CHECK(!r.spectral);
    CHECK(r.caption == cascade::i18n::tr("DEMODULATED AUDIO"));
    CHECK(r.display.empty());
    CHECK(r.centre == "10 ms/DIV");
    CHECK(r.gain == "100 mV/DIV");
    CHECK(r.timebaseKey == "10 ms/DIV");
    CHECK(r.audioFrom == "AUDIO FROM dab-radio");
    CHECK(r.dead.empty());
    // the display word is lettered on the glass whenever it is not NORM
    s.setDisplay(1);
    CHECK(s.readouts().display == cascade::i18n::tr("AVG"));
    s.setDisplay(2);
    CHECK(s.readouts().display == cascade::i18n::tr("PERSIST"));
    // the plugin legend is for the two audio positions only, never on the baseband ones
    s.setSignal(2);
    CHECK(s.readouts().audioFrom.empty());
    s.setSignal(1);
    CHECK(s.readouts().audioFrom == "AUDIO FROM dab-radio");
    // a rate of 0 prints "--" for the time base
    ScriptTaps norate;
    norate.audioRate = 0.0;
    DemodScope z;
    DemodInputs zi = inputs(1.0);
    zi.audioRateHz = 0.0;
    z.tick(norate, zi);
    CHECK(z.readouts().centre == "--");
}

bool bright(const Rgba& p) { return p.r >= 170; }                       // only the beam is this bright (the glass is 9/18/11, the rules at most about 60)
bool lit(const Rgba& p) { return p.g > 150; }                            // any part of the beam, thin spectrum lines included
bool greenish(const Rgba& p) { return p.g > 170 && p.g > p.r + 20; }   // a phosphor trace

void painter() {
    ScriptTaps taps;
    DemodScope s;
    s.state.autoGain = false;
    double now = 6.0;
    warm(s, taps, now);
    constexpr int W = 400, H = 280;
    std::vector<Rgba> a(static_cast<std::size_t>(W) * H), b(a.size());
    PaintScratch scratch;
    const double t0 = nowMs();
    const std::size_t segs = paintDemodFace(a.data(), W, H, s, scratch);
    std::printf("  paint: AUDIO %dx%d %zu segments %.2f ms\n", W, H, segs, nowMs() - t0);
    CHECK(segs > 400);
    CHECK(countWhere(a, bright) > 100);       // the beam is there
    CHECK(countWhere(a, greenish) > 100);
    // deterministic: the same inputs, the same bytes
    paintDemodFace(b.data(), W, H, s, scratch);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Rgba)) == 0);
    // the glass is opaque everywhere
    CHECK(std::all_of(a.begin(), a.end(), [](const Rgba& p) { return p.a == 255; }));
    // the beam keeps inside the glass: a sine of amplitude 0.5 at 100 mV/DIV (4 divisions is 0.4) is clipped to the top and bottom rule, never drawn past them
    // (rows above the clip would be outside the buffer anyway; check the centre row is the axis and the first rows carry beam only where the trace is clipped)
    // a dead tube draws NO beam at all
    taps.writtenAudio += 0;
    DemodScope dead;
    ScriptTaps still;
    dead.tick(still, inputs(1.0));
    dead.tick(still, inputs(1.5));
    CHECK(!dead.live());
    paintDemodFace(b.data(), W, H, dead, scratch);
    CHECK(countWhere(b, bright) == 0);
    CHECK(countWhere(b, greenish) < 50);
    // a canvas under the minimum draws only the glass (a two-division scope would be a ruler that lies)
    std::vector<Rgba> tiny(100 * 90);
    CHECK(paintDemodFace(tiny.data(), 100, 90, s, scratch) == 0);
    CHECK(countWhere(tiny, bright) == 0);
    // each signal paints: the spectrum, the two baseband traces and the vector figure
    for (int sig = 1; sig <= 3; ++sig) {
        DemodScope t;
        t.state.signal = sig;
        t.state.autoGain = false;
        ScriptTaps tt;
        double nt = 2.0;
        warm(t, tt, nt);
        std::fill(b.begin(), b.end(), Rgba{});
        const std::size_t n = paintDemodFace(b.data(), W, H, t, scratch);
        CHECK(n > 200);
        CHECK(countWhere(b, lit) > 30);
    }
    // the I/Q position draws two traces in two colours: a cooler Q beside the phosphor I
    {
        DemodScope t;
        t.state.signal = 2;
        t.state.autoGain = false;
        ScriptTaps tt;
        double nt = 2.0;
        warm(t, tt, nt);
        std::fill(b.begin(), b.end(), Rgba{});
        paintDemodFace(b.data(), W, H, t, scratch);
        const std::size_t cool = countWhere(b, [](const Rgba& p) { return p.b > p.g + 10 && p.b > 120; });
        CHECK(cool > 50);
    }
}

void persistence() {
    // PERSIST on the vector figure keeps a decaying phosphor and the painter's cost stays bounded (the classic strokes up to 245 000 ghost segments a frame; this raster strokes the live figure only)
    ScriptTaps taps;
    DemodScope s;
    s.state.signal = 3;
    s.state.display = 2;
    s.state.autoGain = false;
    constexpr int W = 400, H = 280;
    std::vector<Rgba> px(static_cast<std::size_t>(W) * H);
    PaintScratch scratch;
    double now = 1.0;
    std::size_t worst = 0;
    double worstMs = 0.0;
    for (int i = 0; i < 80; ++i) {
        taps.writtenIq += 6600;
        taps.iqHz = 5000.0f + static_cast<float>(i) * 40.0f;   // each frame's figure differs
        s.tick(taps, inputs(now));
        const double t0 = nowMs();
        worst = std::max(worst, paintDemodFace(px.data(), W, H, s, scratch));
        worstMs = std::max(worstMs, nowMs() - t0);
        now += 0.033;
    }
    std::printf("  paint: VECTOR+PERSIST worst %zu segments, %.2f ms a frame\n", worst, worstMs);
    // the phosphor glows (the figure just drawn is at full intensity) and the cost is bounded: the live figure only, never a ghost per frame
    CHECK(scratch.ghostPeak > 0.9);
    CHECK(!scratch.phosphor.empty());
    CHECK(worstMs < 150.0);   // a guard against the 500 ms of 245 000 ghost strokes, not a benchmark (the figure here is the worst case: 80 turns a sweep, 25 points a turn)
    CHECK(worst < 6000);
    // the figures glow only while the tube is live: the tap stops counting, the tube goes dead after a quarter second and the phosphor is gone (no stale figure comes back later)
    now += 0.4;
    s.tick(taps, inputs(now));
    CHECK(!s.live());
    paintDemodFace(px.data(), W, H, s, scratch);
    CHECK(scratch.phosphor.empty());
    CHECK(scratch.ghostPeak == 0.0);

    // a PERSIST audio trace draws the remembered envelope too: more segments than the live trace alone
    ScriptTaps ta;
    DemodScope p;
    p.state.display = 2;
    p.state.autoGain = false;
    double np = 1.0;
    warm(p, ta, np);
    const std::size_t withMem = paintDemodFace(px.data(), W, H, p, scratch);
    DemodScope q;
    q.state.autoGain = false;
    double nq = 1.0;
    ScriptTaps tq;
    warm(q, tq, nq);
    const std::size_t live = paintDemodFace(px.data(), W, H, q, scratch);
    CHECK(withMem > live);
    // the display mode changing resets the memory
    p.setDisplay(1);
    p.tick(ta, inputs(np + 0.033));
    CHECK(p.memory().ghosts(np + 0.033).empty());
}

void averaging() {
    // AVG blends successive sweeps: a constant feed converges, and the first sweep after a reset is the feed itself
    ScriptTaps taps;
    DemodScope s;
    s.state.display = 1;
    s.state.autoGain = false;
    double now = 1.0;
    warm(s, taps, now);
    const DemodFeed& f = s.feed();
    CHECK(f.audio != nullptr);
    CHECK(f.audioCount == 4800);
    // the averaged feed is the memory's, not the tap buffer: a changed tone shows only part-way after one tick
    taps.audioAmp = 0.2f;
    now += 0.033;
    taps.writtenAudio += 1600;
    s.tick(taps, inputs(now));
    float peak = 0.0f;
    for (std::size_t i = 0; i < s.feed().audioCount; ++i) { peak = std::max(peak, std::abs(s.feed().audio[i])); }
    CHECK(peak > 0.2f);
    CHECK(peak < 0.5f);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    spectrumTransform();
    gatherAudio();
    liveness();
    spectrumAndMultiplex();
    baseband();
    autoRanging();
    keys();
    readoutWords();
    painter();
    persistence();
    averaging();
    return testSummary("test_slint_host_scope");
}
