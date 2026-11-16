// Screen lane S1's pure rules (src/slint/s1_logic.hpp; docs/multi-radio-records/screen-s1-signal.md): the words of the signal meter in each unit, a slider against its range, the passband presets of each mode,
// the tone squelch's lists (50 CTCSS tones, 83 DCS codes) and what a typed tone or code means, the audio filters' choices, the notch slot, the stereo placement's ranges and the engine's state by name.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "s1_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slints1;

int main() {
    std::string e;

    // ---------------------------------------------------------------- a slider against its range
    CHECK(fromLevel(kCwPitch, 0.0) == 300.0 && fromLevel(kCwPitch, 1.0) == 1200.0);
    CHECK(std::fabs(fromLevel(kCwPitch, 0.5) - 750.0) < 1e-9);
    CHECK(fromLevel(kCwPitch, -3.0) == 300.0 && fromLevel(kCwPitch, 7.0) == 1200.0);          // outside the track reads the end
    CHECK(fromLevel(kCwPitch, std::nan("")) == 300.0);                                         // not a number reads the low end
    CHECK(fromLevelStep(kCwPitch, 2.0 / 3.0, 1.0) == 900.0);                                   // the fraction the drive script presses
    CHECK(fromLevelStep(kEqDb, 0.5, 0.5) == 0.0);                                              // the middle is flat
    CHECK(fromLevelStep(kEqDb, 1.0, 0.5) == 12.0 && fromLevelStep(kEqDb, 0.0, 0.5) == -12.0);
    CHECK(std::fabs(fromLevelStep(kEqDb, 0.7, 0.5) - 5.0) < 1e-9);                            // 0.7 * 24 - 12 = 4.8, rounded to the nearest half decibel
    CHECK(std::fabs(toLevel(kCwPitch, 700.0) - 400.0 / 900.0) < 1e-6);
    CHECK(toLevel(kCwPitch, 50.0) == 0.0 && toLevel(kCwPitch, 5000.0) == 1.0 && toLevel(kCwPitch, std::nan("")) == 0.0);
    for (double hz = 300.0; hz <= 1200.0; hz += 100.0) { CHECK(fromLevelStep(kCwPitch, toLevel(kCwPitch, hz), 1.0) == hz); }   // a figure survives a trip through the slider

    // ---------------------------------------------------------------- the meter in each unit
    CHECK(sUnitsText(9.0) == "S9" && sUnitsText(7.0) == "S7" && sUnitsText(7.99) == "S7" && sUnitsText(0.0) == "S0" && sUnitsText(-3.0) == "S0");
    CHECK(sUnitsText(9.0 + 20.0 / 60.0) == "S9+20 dB");                                        // the engine's own figure for S9 + 20 dB is 9.333
    CHECK(sUnitsText(9.0 + 60.0 / 60.0) == "S9+60 dB");
    CHECK(sUnitsText(std::nan("")) == "S0");
    CHECK(meterReading(FOXAPI_METER_DBFS, -62.43, -90.0, 6.0, true) == "-62.4 dB");           // the old reading, unchanged
    CHECK(meterReading(FOXAPI_METER_S, -62.43, -90.0, 6.0, true) == "S6");
    CHECK(meterReading(FOXAPI_METER_S, -62.43, -90.0, 6.0, false) == "~S6");                  // an estimate says so
    CHECK(meterReading(FOXAPI_METER_DBM, -62.43, -87.4, 6.0, true) == "-87 dBm");
    CHECK(meterReading(FOXAPI_METER_DBM, -62.43, -87.4, 6.0, false) == "~-87 dBm");
    CHECK(meterUnitNames().size() == 3);

    // ---------------------------------------------------------------- the passband presets
    CHECK(widthPresets(FOXAPI_DEMOD_NFM) == widthPresets(FOXAPI_DEMOD_WFM) && widthPresets(FOXAPI_DEMOD_NFM).size() == 5);   // the engine's five FM widths
    CHECK(widthPresets(FOXAPI_DEMOD_NFM).front() == 12500.0 && widthPresets(FOXAPI_DEMOD_NFM).back() == 225000.0);
    for (unsigned m = FOXAPI_DEMOD_NFM; m <= FOXAPI_DEMOD_RAW; ++m) {
        const auto p = widthPresets(m);
        CHECK(!p.empty());
        for (std::size_t i = 0; i < p.size(); ++i) { CHECK(p[i] >= 3000.0); if (i > 0) { CHECK(p[i] > p[i - 1]); } }   // the engine holds a width at 3 kHz or more; narrowest first
    }
    CHECK(widthPresets(FOXAPI_DEMOD_AM) != widthPresets(FOXAPI_DEMOD_NFM));                    // a mode has its own list
    CHECK(isFmMode(FOXAPI_DEMOD_NFM) && isFmMode(FOXAPI_DEMOD_WFM) && !isFmMode(FOXAPI_DEMOD_AM) && !isFmMode(FOXAPI_DEMOD_USB));
    CHECK(widthText(12500.0) == "12.5 kHz" && widthText(3000.0) == "3 kHz" && widthText(225000.0) == "225 kHz" && widthText(500.0) == "500 Hz");
    CHECK(presetIndexOf(widthPresets(FOXAPI_DEMOD_NFM), 30000.0) == 1 && presetIndexOf(widthPresets(FOXAPI_DEMOD_NFM), 30000.4) == 1);
    CHECK(presetIndexOf(widthPresets(FOXAPI_DEMOD_NFM), 31000.0) == -1);                       // none of them: nothing is lit
    CHECK(fmWidthNames().size() == 5 && shapeNames().size() == 4);

    // ---------------------------------------------------------------- CTCSS and DCS typed
    double hz = 0; int code = 0;
    CHECK(parseCtcss("88.5", hz, e) && hz == 88.5);
    CHECK(parseCtcss("254.1", hz, e) && hz == 254.1);                                          // an extra tone of the 50
    CHECK(parseCtcss("", hz, e) && hz == 0.0 && parseCtcss("0", hz, e) && hz == 0.0);          // any tone
    CHECK(!parseCtcss("88", hz, e) && !e.empty());                                             // not one of the 50
    CHECK(!parseCtcss("abc", hz, e) && !parseCtcss("-5", hz, e) && !parseCtcss("999", hz, e));
    CHECK(parseDcs("023", code, e) && code == 23);
    CHECK(parseDcs("23", code, e) && code == 23);
    CHECK(parseDcs("754", code, e) && code == 754);
    CHECK(parseDcs("", code, e) && code == 0 && parseDcs("0", code, e) && code == 0);
    CHECK(!parseDcs("024", code, e) && !e.empty());                                            // three octal digits but not one of the 83
    CHECK(!parseDcs("089", code, e) && !parseDcs("1234", code, e) && !parseDcs("x", code, e));
    CHECK(dcsText(23) == "023" && dcsText(754) == "754" && ctcssText(67.0) == "67.0");
    {   // stepping walks all 50 tones and comes round; from "any" it starts at the ends
        double t = 0.0;
        t = nextCtcss(t, 1);
        CHECK(t == 67.0);
        int steps = 1;
        for (; steps < 60 && t != 254.1; ++steps) { t = nextCtcss(t, 1); }
        CHECK(steps == 50);                                                                    // 50 tones, ascending, and 254.1 is the last
        CHECK(nextCtcss(254.1, 1) == 67.0);                                                    // round the end
        CHECK(nextCtcss(67.0, -1) == 254.1);
        CHECK(nextCtcss(0.0, -1) == 254.1);
        CHECK(nextCtcss(88.5, 1) > 88.5 && nextCtcss(88.5, -1) < 88.5);
        int c = 0, count = 0;
        c = nextDcs(c, 1);
        CHECK(c == 23);
        for (; count < 100; ++count) { const int n = nextDcs(c, 1); if (n == 23) { break; } c = n; }
        CHECK(count == 82);                                                                    // 83 codes and the last steps back to the first
        CHECK(nextDcs(23, -1) == 754);
        CHECK(nextDcs(0, -1) == 754);
    }
    CHECK(toneKindNames().size() == 3 && polarityNames().size() == 3);

    // ---------------------------------------------------------------- the audio filters' choices
    CHECK(indexOfHighPass(0) == 0 && indexOfHighPass(100) == 1 && indexOfHighPass(300) == 2 && indexOfHighPass(200) == -1);
    CHECK(indexOfLowPass(0) == 0 && indexOfLowPass(2400) == 1 && indexOfLowPass(3000) == 2 && indexOfLowPass(6000) == 3 && indexOfLowPass(4000) == -1);
    CHECK(highPassNames().size() == 3 && lowPassNames().size() == 4 && nrMethodNames().size() == 3 && placementNames().size() == 4 && eqBandNames().size() == 8);
    CHECK(kHighPassHz[2] == 300 && kLowPassHz[3] == 6000);

    // ---------------------------------------------------------------- stereo placement
    CHECK(placementRange(1).lo == -1.0 && placementRange(1).hi == 1.0);
    CHECK(placementRange(2).lo == 0.0 && placementRange(2).hi == 1.0);
    CHECK(placementRange(3).lo == -90.0 && placementRange(3).hi == 90.0);
    CHECK(placementDefault(1) == 0.0 && placementDefault(2) == 0.5 && placementDefault(3) == 0.0);
    CHECK(placementText(2, 0.5) == "50%");
    CHECK(placementText(3, 30.0).rfind("+30 ", 0) == 0 && placementText(3, -30.0).rfind("-30 ", 0) == 0);   // a signed angle, then the word for degrees
    CHECK(!placementText(1, -1.0).empty() && placementText(1, 0.0) != placementText(1, 0.5));

    // ---------------------------------------------------------------- the notch bank
    int slot = 0;
    CHECK(parseNotchSlot("1", slot, e) && slot == 0);
    CHECK(parseNotchSlot("10", slot, e) && slot == 9);
    CHECK(!parseNotchSlot("0", slot, e) && !parseNotchSlot("11", slot, e) && !parseNotchSlot("2.5", slot, e) && !parseNotchSlot("", slot, e) && !e.empty());
    CHECK(parseNotchHz("20", hz, e) && hz == 20.0 && parseNotchHz("21600", hz, e) && hz == 21600.0);
    CHECK(!parseNotchHz("19.9", hz, e) && !parseNotchHz("21600.1", hz, e) && !parseNotchHz("x", hz, e));
    CHECK(parseNotchWidth("1", hz, e) && parseNotchWidth("2000", hz, e) && !parseNotchWidth("0.5", hz, e) && !parseNotchWidth("2001", hz, e));
    {
        double h[10] = {1000, 440, 0, 0, 0, 0, 0, 0, 0, 0}, w[10] = {50, 20, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(notchSummary(0u, h, w).empty());                                                 // none on
        const std::string both = notchSummary(0b11u, h, w);
        CHECK(both.find("1: 1000 Hz") == 0 && both.find("2: 440 Hz") != std::string::npos && both.find(';') != std::string::npos);
        const std::string only2 = notchSummary(0b10u, h, w);
        CHECK(only2.find("1:") == std::string::npos && only2.find("2: 440 Hz") == 0);          // a slot that is off is not listed
    }

    // ---------------------------------------------------------------- the I/Q correction
    CHECK(kIqDc == 1u && kIqBalance == 2u && kIqSwap == 4u);                                   // the engine's own bits (SET_IQ_CORRECT)
    CHECK(imageRejectionText(200.0).empty() && imageRejectionText(std::nan("")).empty());      // the engine says 200 while nothing is measured
    CHECK(imageRejectionText(34.25) == "34.2 dB" || imageRejectionText(34.25) == "34.3 dB");

    // ---------------------------------------------------------------- the passband's edges and the AM detector (the engine's facts)
    {
        const PassbandFact a = parsePassbandFact("1;500;2000;200;1");
        CHECK(a.ok && a.on && a.active && a.low == 500.0 && a.high == 2000.0 && a.shift == 200.0);
        const PassbandFact b = parsePassbandFact("0;300;2700;-100.5;0");
        CHECK(b.ok && !b.on && !b.active && b.shift == -100.5);
        CHECK(!parsePassbandFact("").ok && !parsePassbandFact("1;2;3").ok && !parsePassbandFact("1;2;3;4;5;6").ok);
        double lo = 0, hi = 0;
        defaultEdges(FOXAPI_DEMOD_USB, 700.0, lo, hi);
        CHECK(lo == 300.0 && hi == 2700.0);
        defaultEdges(FOXAPI_DEMOD_LSB, 700.0, lo, hi);
        CHECK(lo == 300.0 && hi == 2700.0);
        defaultEdges(FOXAPI_DEMOD_CW, 700.0, lo, hi);
        CHECK(lo == 450.0 && hi == 950.0);                                                        // the pitch plus and minus 250 Hz
        defaultEdges(FOXAPI_DEMOD_CW, 300.0, lo, hi);
        CHECK(lo == 50.0 && hi == 550.0);
        defaultEdges(FOXAPI_DEMOD_AM, 700.0, lo, hi);
        CHECK(lo == 4500.0 && hi == 4500.0);
        CHECK(hasPassband(FOXAPI_DEMOD_USB) && hasPassband(FOXAPI_DEMOD_LSB) && hasPassband(FOXAPI_DEMOD_CW) && hasPassband(FOXAPI_DEMOD_AM));
        CHECK(!hasPassband(FOXAPI_DEMOD_NFM) && !hasPassband(FOXAPI_DEMOD_WFM) && !hasPassband(FOXAPI_DEMOD_DSB) && !hasPassband(FOXAPI_DEMOD_RAW));
        CHECK(checkCuts(FOXAPI_DEMOD_USB, 300.0, 2700.0, e));
        CHECK(!checkCuts(FOXAPI_DEMOD_USB, 2700.0, 300.0, e) && !e.empty());                        // the high cut above the low
        CHECK(!checkCuts(FOXAPI_DEMOD_CW, 500.0, 500.0, e));
        CHECK(checkCuts(FOXAPI_DEMOD_AM, 6000.0, 3000.0, e));                                       // in AM the two are the extents each side of the carrier, in any order
        const AmDetFact d = parseAmDetFact("2;1;57.5;0.42");
        CHECK(d.ok && d.kind == 2 && d.locked && d.offsetHz == 57.5 && std::fabs(d.presence - 0.42) < 1e-9);
        CHECK(!parseAmDetFact("").ok && !parseAmDetFact("4;0;0;0").ok && !parseAmDetFact("0;0;0").ok);
        CHECK(amDetectorNames().size() == 4);
    }

    // ---------------------------------------------------------------- the engine's state by name
    {
        FoxReceiverState s{};
        s.cwPitchHz = 640.0; s.eqDb[3] = -4.5; s.notchBankHz[2] = 1234.0; s.notchBankWidthHz[9] = 77.0; s.meterUnits = FOXAPI_METER_DBM; s.iqCorrectMask = 5u; s.stereoPlaceValue = -0.25;
        double v = 0;
        CHECK(stateValue(s, "cwPitchHz", v) && v == 640.0);
        CHECK(stateValue(s, "eqDb.3", v) && v == -4.5);
        CHECK(stateValue(s, "notchBankHz.2", v) && v == 1234.0);
        CHECK(stateValue(s, "notchBankWidthHz.9", v) && v == 77.0);
        CHECK(stateValue(s, "meterUnits", v) && v == 2.0);
        CHECK(stateValue(s, "iqCorrectMask", v) && v == 5.0);
        CHECK(stateValue(s, "stereoPlaceValue", v) && v == -0.25);
        CHECK(!stateValue(s, "eqDb.8", v) && !stateValue(s, "eqDb.-1", v) && !stateValue(s, "notchBankHz.10", v) && !stateValue(s, "nothing", v));
    }
    return testSummary("test_slint_s1_host");
}
