// Tests for core/freq_converter.hpp - the one arithmetic between the AIR
// frequency (where the signal is, and the only frequency the user sees) and
// the RADIO frequency (what the radio is told) when an up- or down-converter
// sits in front of the radio.
//
// The worked examples are the beta tester's own: SAQ Grimeton on 17.2 kHz and
// a CW preset at 16.4 kHz, through home-built up-converters with 125 MHz,
// 100 MHz and 2 MHz local oscillators.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

#include "core/freq_converter.hpp"
#include "test_check.hpp"

using cascade::core::airFromRadio;
using cascade::core::airRangeHz;
using cascade::core::airReachable;
using cascade::core::conjugateInPlace;
using cascade::core::ConverterMode;
using cascade::core::ConverterSetting;
using cascade::core::converterActive;
using cascade::core::converterFor;
using cascade::core::converterHzText;
using cascade::core::converterMirrors;
using cascade::core::converterModeFromKey;
using cascade::core::converterModeKey;
using cascade::core::converterRadioKey;
using cascade::core::kMaxConverterRadios;
using cascade::core::parseConverterLoHz;
using cascade::core::radioFromAir;
using cascade::core::sanitiseConverter;
using cascade::core::sanitiseConverters;

namespace {

ConverterSetting up(double lo, bool inv = false) { return {ConverterMode::Up, lo, inv}; }
ConverterSetting down(double lo, bool inv = false) { return {ConverterMode::Down, lo, inv}; }

void testOffIsIdentity() {
    std::printf("  off: radio = air, both ways, whatever the LO field holds\n");
    const ConverterSetting off{};
    CHECK(!converterActive(off));
    CHECK(radioFromAir(off, 100.0e6) == 100.0e6);
    CHECK(airFromRadio(off, 100.0e6) == 100.0e6);
    // An LO left in the field while the mode is Off changes nothing.
    const ConverterSetting offWithLo{ConverterMode::Off, 125.0e6, true};
    CHECK(!converterActive(offWithLo));
    CHECK(!converterMirrors(offWithLo));
    CHECK(radioFromAir(offWithLo, 16400.0) == 16400.0);
    // A mode with no valid LO is OFF: a half-typed setting never shifts a tune.
    CHECK(!converterActive(up(0.0)));
    CHECK(radioFromAir(up(0.0), 16400.0) == 16400.0);
    CHECK(!converterActive(up(std::numeric_limits<double>::quiet_NaN())));
    CHECK(!converterActive(up(-125.0e6)));
    CHECK(!converterActive(up(301.0e9)));
    CHECK(airReachable(off, 16400.0));
}

void testSaqExamples() {
    std::printf("  the tester's own numbers: 16.4 kHz and 17.2 kHz through 125 / 100 / 2 MHz\n");
    // "a CW preset at 16.4 kHz must tune the radio to 125.0164 MHz with a
    // 125 MHz up-converter"
    CHECK(radioFromAir(up(125.0e6), 16400.0) == 125016400.0);
    CHECK(radioFromAir(up(125.0e6), 17200.0) == 125017200.0);  // SAQ Grimeton
    CHECK(radioFromAir(up(100.0e6), 16400.0) == 100016400.0);
    CHECK(radioFromAir(up(2.0e6), 16400.0) == 2016400.0);
    CHECK(radioFromAir(up(2.0e6), 17200.0) == 2017200.0);
    // ...and the radio's readback comes back as the air frequency.
    CHECK(airFromRadio(up(125.0e6), 125016400.0) == 16400.0);
    CHECK(airFromRadio(up(2.0e6), 2017200.0) == 17200.0);
    CHECK(airReachable(up(125.0e6), 16400.0));
    CHECK(airReachable(up(2.0e6), 0.0));  // DC in, 2 MHz at the radio
}

void testDownAndInverted() {
    std::printf("  down-converter (LNB), and the inverted (high-side LO) product\n");
    // A 9.75 GHz LNB: QO-100's 10.489 GHz narrowband transponder at 739 MHz.
    CHECK_NEAR(radioFromAir(down(9.75e9), 10.4895e9), 739.5e6, 1e-3);
    CHECK_NEAR(airFromRadio(down(9.75e9), 739.5e6), 10.4895e9, 1e-3);
    // Inverted: radio = LO - air, for either mode.
    CHECK(radioFromAir(up(125.0e6, true), 16400.0) == 124983600.0);
    CHECK(airFromRadio(up(125.0e6, true), 124983600.0) == 16400.0);
    CHECK(radioFromAir(down(10.0e9, true), 9.5e9) == 500.0e6);
    CHECK(airFromRadio(down(10.0e9, true), 500.0e6) == 9.5e9);
    CHECK(converterMirrors(up(125.0e6, true)));
    CHECK(!converterMirrors(up(125.0e6, false)));
    CHECK(!converterMirrors(ConverterSetting{ConverterMode::Off, 125.0e6, true}));
    // Inversion means the air frequency goes DOWN when the radio goes up.
    const ConverterSetting inv = up(125.0e6, true);
    CHECK(airFromRadio(inv, 124990000.0) < airFromRadio(inv, 124980000.0));
}

void testRoundTrips() {
    std::printf("  round trips: air -> radio -> air is exact over a sweep of settings\n");
    const ConverterSetting settings[] = {up(125.0e6), up(100.0e6), up(2.0e6),       up(125.0e6, true),
                                         down(9.75e9), down(10.0e9, true), up(116.0e6), down(2.4e9)};
    const double airs[] = {0.0, 16400.0, 17200.0, 77500.0, 1.0e6, 7.1e6, 10.4895e9};
    int exact = 0;
    int total = 0;
    for (const ConverterSetting& s : settings) {
        for (const double a : airs) {
            ++total;
            const double back = airFromRadio(s, radioFromAir(s, a));
            if (back == a) { ++exact; }
            CHECK(std::fabs(back - a) <= 1e-6 * std::max(1.0, std::fabs(a)));
            const double r = 125.123456e6;
            CHECK(std::fabs(radioFromAir(s, airFromRadio(s, r)) - r) <= 1e-6 * r);
        }
    }
    std::printf("      %d of %d exact to the last bit\n", exact, total);
}

void testLimits() {
    std::printf("  limits: what a converter cannot reach, and the air range a radio covers\n");
    // A down-converter cannot deliver an air frequency below its LO: the
    // radio would have to tune below 0 Hz.
    CHECK(!airReachable(down(9.75e9), 100.0e6));
    CHECK(!airReachable(down(9.75e9), 9.75e9));   // exactly 0 Hz at the radio
    CHECK(airReachable(down(9.75e9), 9.76e9));
    // An inverted converter cannot deliver an air frequency at or above its LO.
    CHECK(!airReachable(up(125.0e6, true), 125.0e6));
    CHECK(airReachable(up(125.0e6, true), 124.0e6));
    // A NEGATIVE AIR CENTRE IS REACHABLE when the radio frequency is not: a
    // 16.4 kHz preset with the VFO parked +300 kHz puts the centre at -283.6
    // kHz, which a 125 MHz up-converter delivers at 124.7164 MHz. (The first
    // scripted run of this feature refused exactly that preset.)
    CHECK(airReachable(up(125.0e6), 16400.0 - 300000.0));
    CHECK(radioFromAir(up(125.0e6), 16400.0 - 300000.0) == 124716400.0);
    CHECK(!airReachable(up(125.0e6), -125.0e6));  // exactly 0 Hz at the radio
    CHECK(!airReachable(up(125.0e6), std::numeric_limits<double>::quiet_NaN()));

    // THE TUNING CONTROLS' FLOOR (the counter's wheel, its switches, the keys).
    using cascade::core::minTunedAirHz;
    // No converter: never below 0, never below the offset - as it always was.
    CHECK(minTunedAirHz(ConverterSetting{}, 300000.0) == 300000.0);
    CHECK(minTunedAirHz(ConverterSetting{}, -300000.0) == 0.0);
    // Up-converter: the tester can wheel down to 0 Hz with the VFO parked up.
    CHECK(minTunedAirHz(up(125.0e6), 300000.0) == 0.0);
    // ...but not to where the RADIO would reach 0 Hz, with a huge offset.
    CHECK(minTunedAirHz(up(2.0e6), 3.0e6) == 1.0e6);
    // Down-converter: nothing below the LO (plus the offset) reaches the radio.
    CHECK(minTunedAirHz(down(9.75e9), 0.0) == 9.75e9);
    CHECK(minTunedAirHz(down(9.75e9), 250000.0) == 9.75e9 + 250000.0);
    // Inverted: its limit is at the top; the floor is 0 Hz.
    CHECK(minTunedAirHz(up(125.0e6, true), 300000.0) == 0.0);

    double lo = 0.0;
    double hi = 0.0;
    // An R820T dongle (24 - 1766 MHz) behind a 125 MHz up-converter: air 0 Hz
    // (clipped: 24 MHz at the radio would be -101 MHz on the air) to 1641 MHz.
    CHECK(airRangeHz(up(125.0e6), true, 24.0e6, 1766.0e6, lo, hi));
    CHECK(lo == 0.0);
    CHECK(hi == 1641.0e6);
    // Behind a 2 MHz converter the dongle's 24 MHz floor is 22 MHz on the air:
    // the tester's 16.4 kHz is out of reach, and the range says why.
    CHECK(airRangeHz(up(2.0e6), true, 24.0e6, 1766.0e6, lo, hi));
    CHECK(lo == 22.0e6);
    CHECK(hi == 1764.0e6);
    // Inverted: the ends swap.
    CHECK(airRangeHz(up(125.0e6, true), true, 24.0e6, 1766.0e6, lo, hi));
    CHECK(lo == 0.0);
    CHECK(hi == 101.0e6);
    // An LNB in front of a 70 MHz - 6 GHz B200.
    CHECK(airRangeHz(down(9.75e9), true, 70.0e6, 6.0e9, lo, hi));
    CHECK(lo == 9.82e9);
    CHECK(hi == 15.75e9);
    // A radio with no published range: the converter's own limit still holds.
    CHECK(airRangeHz(down(9.75e9), false, 0.0, 0.0, lo, hi));
    CHECK(lo == 9.75e9);
    CHECK(std::isinf(hi));
    CHECK(airRangeHz(up(125.0e6, true), false, 0.0, 0.0, lo, hi));
    CHECK(lo == 0.0);
    CHECK(hi == 125.0e6);
    // Off: the radio's own range, or nothing when it has none.
    CHECK(airRangeHz(ConverterSetting{}, true, 24.0e6, 1766.0e6, lo, hi));
    CHECK(lo == 24.0e6);
    CHECK(hi == 1766.0e6);
    CHECK(!airRangeHz(ConverterSetting{}, false, 0.0, 0.0, lo, hi));
    // A range that maps wholly below 0 Hz on the air covers nothing.
    CHECK(!airRangeHz(up(125.0e6, true), true, 200.0e6, 300.0e6, lo, hi));
}

void testConjugate() {
    std::printf("  the mirror: conjugating moves a tone from +f to -f\n");
    // A tone at +1/8 of the rate: after the mirror its phase must advance
    // by -pi/4 per sample.
    std::complex<float> s[16];
    for (int i = 0; i < 16; ++i) {
        const double ph = 2.0 * 3.14159265358979 * 0.125 * i;
        s[i] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    conjugateInPlace(s, 16);
    for (int i = 1; i < 16; ++i) {
        const double step = std::arg(s[i] * std::conj(s[i - 1]));
        CHECK_NEAR(step, -3.14159265358979 / 4.0, 1e-4);
    }
}

void testPersistenceWords() {
    std::printf("  config words: off/up/down, anything else is off\n");
    CHECK(std::string(converterModeKey(ConverterMode::Off)) == "off");
    CHECK(std::string(converterModeKey(ConverterMode::Up)) == "up");
    CHECK(std::string(converterModeKey(ConverterMode::Down)) == "down");
    CHECK(converterModeFromKey("up") == ConverterMode::Up);
    CHECK(converterModeFromKey("down") == ConverterMode::Down);
    CHECK(converterModeFromKey("off") == ConverterMode::Off);
    CHECK(converterModeFromKey("UP") == ConverterMode::Off);
    CHECK(converterModeFromKey("sideways") == ConverterMode::Off);
    CHECK(converterModeFromKey("") == ConverterMode::Off);

    // An invalid LO turns the converter off and forgets the LO.
    const ConverterSetting bad = sanitiseConverter({ConverterMode::Up, -5.0, true});
    CHECK(bad.mode == ConverterMode::Off);
    CHECK(bad.loHz == 0.0);
    const ConverterSetting good = sanitiseConverter(up(125.0e6, true));
    CHECK(good == up(125.0e6, true));
}

void testPerRadioKeys() {
    std::printf("  per radio: keys, lookups, and the generator and files kept apart\n");
    CHECK(converterRadioKey("siggen", "") == "siggen");
    CHECK(converterRadioKey("", "") == "siggen");
    CHECK(converterRadioKey("file", "C:/x.wav") == "file");
    CHECK(converterRadioKey("rtlsdr", "serial=00000001") == "rtlsdr|serial=00000001");
    CHECK(converterRadioKey("soapy", "driver=uhd") == "soapy|driver=uhd");

    std::map<std::string, ConverterSetting> all;
    all["rtlsdr|serial=00000001"] = up(125.0e6);
    all["rtlsdr|serial=00000002"] = up(2.0e6);
    CHECK(converterFor(all, "rtlsdr|serial=00000001") == up(125.0e6));
    CHECK(converterFor(all, "rtlsdr|serial=00000002") == up(2.0e6));
    // A radio nobody set one for, the generator and a file: OFF.
    CHECK(!converterActive(converterFor(all, "rtlsdr|serial=00000003")));
    CHECK(!converterActive(converterFor(all, "siggen")));
    CHECK(!converterActive(converterFor(all, "file")));
    // A bad remembered value reads as off rather than as a shift.
    all["hackrf|serial=x"] = ConverterSetting{ConverterMode::Down, 0.0, false};
    CHECK(!converterActive(converterFor(all, "hackrf|serial=x")));

    std::map<std::string, ConverterSetting> big;
    for (std::size_t i = 0; i < kMaxConverterRadios + 10; ++i) {
        big["rtlsdr|serial=" + std::to_string(1000 + i)] = up(125.0e6);
    }
    big[""] = up(125.0e6);
    const auto clean = sanitiseConverters(big);
    CHECK(clean.size() == kMaxConverterRadios);
    CHECK(clean.count("") == 0);
}

void testText() {
    std::printf("  text: frequencies without trailing zeros, and the typed LO\n");
    CHECK(converterHzText(125.0e6) == "125 MHz");
    CHECK(converterHzText(2.0e6) == "2 MHz");
    CHECK(converterHzText(100.0e6) == "100 MHz");
    CHECK(converterHzText(16400.0) == "16.4 kHz");
    CHECK(converterHzText(125016400.0) == "125.0164 MHz");
    CHECK(converterHzText(9.75e9) == "9.75 GHz");
    CHECK(converterHzText(0.0) == "0 Hz");
    CHECK(converterHzText(999.0) == "999 Hz");

    double hz = -1.0;
    CHECK(parseConverterLoHz("125", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("125 MHz", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("125M", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("125mhz", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("125000000", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("125,000,000 Hz", hz) && hz == 125.0e6);
    CHECK(parseConverterLoHz("100000 kHz", hz) && hz == 100.0e6);
    CHECK(parseConverterLoHz("2000k", hz) && hz == 2.0e6);
    CHECK(parseConverterLoHz("9.75 GHz", hz) && hz == 9.75e9);
    CHECK(parseConverterLoHz("2", hz) && hz == 2.0e6);
    hz = 42.0;
    CHECK(!parseConverterLoHz("", hz));
    CHECK(!parseConverterLoHz("abc", hz));
    CHECK(!parseConverterLoHz("125 MHzz", hz));
    CHECK(!parseConverterLoHz("-125", hz));
    CHECK(!parseConverterLoHz("0", hz));
    CHECK(!parseConverterLoHz("400 GHz", hz));
    CHECK(hz == 42.0);  // untouched by every refusal
}

// THE LO IS WHOLE HERTZ, wherever it comes from. A driver stores what it is
// told in whole hertz (an RTL-SDR's tuner call takes a uint32), so an LO with
// a fraction in it tells the radio a fractional frequency, the radio keeps the
// whole part, and the readback comes back as an air frequency the user never
// asked for: 16399.543 Hz for a 16.4 kHz tune. The counter then disagrees with
// itself, and the repeat-tune guard (applyRetuneNow: "no-op when the tune does
// not move anything") never matches, so every repeated command re-tunes the
// radio and resets the decoders.
void testLoIsWholeHertz() {
    std::printf("  the LO is rounded to whole hertz, typed or loaded\n");
    // TYPED: what the Local oscillator field hands to the setting.
    double hz = 0.0;
    CHECK(parseConverterLoHz("124.99812345 MHz", hz));
    CHECK(hz == 124998123.0);
    CHECK(parseConverterLoHz("124998123.6", hz));  // a bare number above 7500 is Hz
    CHECK(hz == 124998124.0);
    CHECK(parseConverterLoHz("9.7501234567 GHz", hz));
    CHECK(hz == 9750123457.0);
    // Half a hertz rounds to 1 Hz, the least LO there is; less rounds to
    // nothing, which is no LO at all.
    CHECK(parseConverterLoHz("0.5 Hz", hz));
    CHECK(hz == 1.0);
    hz = 42.0;
    CHECK(!parseConverterLoHz("0.4 Hz", hz));
    CHECK(hz == 42.0);

    // LOADED: sanitiseConverter is what every loaded (and every changed)
    // setting passes through.
    const ConverterSetting s = sanitiseConverter({ConverterMode::Up, 124998123.45678912, false});
    CHECK(s.loHz == 124998123.0);
    CHECK(s.mode == ConverterMode::Up);
    CHECK(sanitiseConverters({{"rtlsdr|serial=1", {ConverterMode::Down, 9750123456.789, true}}})
              .at("rtlsdr|serial=1")
              .loHz == 9750123457.0);
    CHECK(sanitiseConverter({ConverterMode::Up, 0.4, false}).mode == ConverterMode::Off);

    // AND WHAT THAT BUYS: a radio that keeps whole hertz reads back exactly
    // the air frequency it was asked for - including air figures of ten and
    // eleven digits, the band centre below 0 Hz, and a 1 Hz step.
    const ConverterSetting settings[] = {
        s, sanitiseConverter({ConverterMode::Up, 124998123.45678912, true}),
        sanitiseConverter({ConverterMode::Down, 9750123456.789, false})};
    const double airs[] = {16400.0,      17200.0,      16401.0,      -283600.0,
                           1296123457.0, 10489123457.0, 11700123457.0, 144800000.0};
    for (const ConverterSetting& c : settings) {
        for (const double air : airs) {
            if (!airReachable(c, air)) { continue; }
            const double told = std::round(radioFromAir(c, air));  // what the radio keeps
            CHECK(airFromRadio(c, told) == air);
        }
    }
}

}  // namespace

int main() {
    std::printf("test_freq_converter\n");
    testLoIsWholeHertz();
    testOffIsIdentity();
    testSaqExamples();
    testDownAndInverted();
    testRoundTrips();
    testLimits();
    testConjugate();
    testPersistenceWords();
    testPerRadioKeys();
    testText();
    return testSummary("test_freq_converter");
}
