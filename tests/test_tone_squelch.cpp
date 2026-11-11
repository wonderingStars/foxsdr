// test_tone_squelch.cpp - THE TONE SQUELCH'S SETTINGS AND ITS GATE (engine wiring lane 1, group B).
//
//   A  the rules of dsp/tone_squelch.hpp: a CTCSS tone is one of the 50 (or 0 = any), a DCS code is written as digits and is one of the 83 (or 0 = any), digits to the octal
//      value and back, a digit past 7 or a number past 777 refused, a polarity 0..2;
//   B  Squelch's tone gate (dsp/squelch.hpp): with no tone required it is the squelch it always was (bit for bit); with a tone required the audio is let through only
//      while the carrier is open AND the tone is present; carrierOpen() stays the carrier alone; the ramp is the carrier gate's (no step at the tone's edges); and a
//      tone that is absent keeps the audio at exact digital silence.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dsp/squelch.hpp"
#include "dsp/tone_squelch.hpp"
#include "test_check.hpp"

using cascade::dsp::Squelch;
namespace tone = cascade::dsp::tone;

namespace {

void testRules() {
    std::printf("A: the rules\n");
    // CTCSS: every one of the 50, and 0; nothing in between
    CHECK(tone::validCtcssHz(0.0));
    for (int i = 0; i < cascade::dsp::kCtcssAllCount; ++i) { CHECK(tone::validCtcssHz(cascade::dsp::ctcssToneHz(i))); }
    for (const double bad : {-67.0, 66.0, 67.2, 100.5, 300.0, 1000.0, 1e9, std::nan(""), HUGE_VAL}) { CHECK(!tone::validCtcssHz(bad)); }
    CHECK(tone::validCtcssHz(67.0));
    CHECK(tone::validCtcssHz(254.1));
    CHECK(tone::validCtcssHz(159.8));   // (one of the 11 extras)
    // DCS digits: 23 is 023 octal = 19
    unsigned oct = 0;
    CHECK(tone::digitsToOctal(23, oct) && oct == 023u);
    CHECK(tone::digitsToOctal(754, oct) && oct == 0754u);
    CHECK(tone::digitsToOctal(0, oct) && oct == 0u);
    CHECK(tone::digitsToOctal(777, oct) && oct == 0777u);
    for (const int bad : {-1, 8, 18, 80, 108, 180, 778, 800, 1000, 99999}) { CHECK(!tone::digitsToOctal(bad, oct)); }
    for (unsigned o = 0; o <= 0777u; ++o) {
        const int digits = tone::octalToDigits(o);
        unsigned back = 0;
        CHECK(tone::digitsToOctal(digits, back) && back == o);   // the two conversions are inverse over all 512 codes
    }
    // the 83: each valid, written as digits; the rest of the 512 are not
    int valid = 0;
    for (int d = 0; d <= 777; ++d) {
        unsigned o = 0;
        if (!tone::digitsToOctal(d, o)) { continue; }
        if (d != 0 && tone::validDcsDigits(d)) {
            ++valid;
            CHECK(cascade::dsp::dcsIsStandardCode(o));
        }
        if (d != 0 && !cascade::dsp::dcsIsStandardCode(o)) { CHECK(!tone::validDcsDigits(d)); }
    }
    CHECK(valid == 83);
    CHECK(tone::validDcsDigits(0));
    CHECK(tone::validDcsDigits(23));
    CHECK(tone::validDcsDigits(754));
    CHECK(!tone::validDcsDigits(24));    // 024 is not a standard code
    CHECK(!tone::validDcsDigits(19));    // 19 would be the octal VALUE 023 written in decimal: the digits are what is written, 19 has a 9
    CHECK(!tone::validDcsDigits(-23));
    CHECK(tone::validPolarity(0) && tone::validPolarity(1) && tone::validPolarity(2));
    CHECK(!tone::validPolarity(-1) && !tone::validPolarity(3));
}

// A closed gate gives exact digital silence; returns the largest absolute output.
float runBlock(Squelch& sq, const std::complex<float>& carrier, std::size_t n, std::vector<float>* outAudio = nullptr) {
    std::vector<std::complex<float>> in(n, carrier);
    std::vector<float> audio(n, 0.5f);
    sq.process(in.data(), n, audio.data());
    float mx = 0.0f;
    for (float a : audio) { mx = std::max(mx, std::fabs(a)); }
    if (outAudio != nullptr) { *outAudio = audio; }
    return mx;
}

void testGate() {
    std::printf("B: the squelch's tone gate\n");
    const double rate = 48000.0;
    const std::complex<float> loud(0.5f, 0.0f);
    const std::complex<float> quiet(0.0f, 0.0f);

    // NOT REQUIRED: identical to a squelch that never heard of the tone gate, sample for sample (the default).
    {
        Squelch a(rate), b(rate);
        a.setThresholdDb(-20.0f);
        b.setThresholdDb(-20.0f);
        b.setToneGate(false, false);   // not required: the tone verdict is ignored
        std::vector<float> oa, ob;
        for (int blk = 0; blk < 20; ++blk) {
            const std::complex<float>& c = (blk % 7 < 4) ? loud : quiet;
            runBlock(a, c, 480, &oa);
            runBlock(b, c, 480, &ob);
            CHECK(oa == ob);
            CHECK(a.isOpen() == b.isOpen());
            CHECK(a.carrierOpen() == a.isOpen());
        }
    }
    // REQUIRED, TONE ABSENT: the carrier opens but the audio stays at exact digital silence
    {
        Squelch s(rate);
        s.setThresholdDb(-20.0f);
        s.setToneGate(true, false);
        float mx = 0.0f;
        for (int blk = 0; blk < 10; ++blk) { mx = std::max(mx, runBlock(s, loud, 480)); }
        CHECK(s.carrierOpen());
        CHECK(!s.isOpen());
        CHECK(mx == 0.0f);
    }
    // REQUIRED, TONE PRESENT: audio passes, and fades in on the ramp (no step)
    {
        Squelch s(rate);
        s.setThresholdDb(-20.0f);
        s.setToneGate(true, true);
        std::vector<float> o;
        runBlock(s, loud, 480, &o);
        CHECK(s.isOpen());
        CHECK(o[0] < 0.05f);                       // starts at the bottom of the ramp
        CHECK(o[479] == 0.5f);                     // and is fully open by the end of the block
        for (std::size_t i = 1; i < o.size(); ++i) { CHECK(o[i] - o[i - 1] < 0.02f); }   // no step anywhere: a 5 ms ramp is 240 samples
    }
    // THE TONE ENDS while the carrier stays: the audio fades out on the same ramp, the carrier gate stays open
    {
        Squelch s(rate);
        s.setThresholdDb(-20.0f);
        s.setToneGate(true, true);
        for (int blk = 0; blk < 5; ++blk) { runBlock(s, loud, 480); }
        CHECK(s.isOpen());
        s.setToneGate(true, false);
        std::vector<float> o;
        runBlock(s, loud, 480, &o);
        CHECK(!s.isOpen());
        CHECK(s.carrierOpen());
        CHECK(o[0] > 0.45f);
        CHECK(o[479] == 0.0f);
        for (std::size_t i = 1; i < o.size(); ++i) { CHECK(o[i - 1] - o[i] < 0.02f); }
    }
    // THE CARRIER GOES while the tone says present: closed all the same (the tone cannot open a squelch the carrier has closed)
    {
        Squelch s(rate);
        s.setThresholdDb(-20.0f);
        s.setToneGate(true, true);
        for (int blk = 0; blk < 5; ++blk) { runBlock(s, loud, 480); }
        float mx = 0.0f;
        for (int blk = 0; blk < 40; ++blk) { mx = std::max(mx, runBlock(s, quiet, 480) * (blk > 30 ? 1.0f : 0.0f)); }
        CHECK(!s.carrierOpen());
        CHECK(!s.isOpen());
        CHECK(mx == 0.0f);
    }
    // switching the requirement off again restores the carrier-only squelch at once
    {
        Squelch s(rate);
        s.setThresholdDb(-20.0f);
        s.setToneGate(true, false);
        for (int blk = 0; blk < 5; ++blk) { runBlock(s, loud, 480); }
        CHECK(!s.isOpen());
        s.setToneGate(false, false);
        for (int blk = 0; blk < 3; ++blk) { runBlock(s, loud, 480); }
        CHECK(s.isOpen());
    }
}

}  // namespace

int main() {
    std::printf("test_tone_squelch\n");
    testRules();
    testGate();
    return testSummary("test_tone_squelch");
}
