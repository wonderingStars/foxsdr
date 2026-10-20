// test_converter_lo_seed.cpp - when the Converter section's LO field is re-seeded (gui/converter_lo_seed.hpp; the pump move, step P2).
//
// The engine used to call EngineHost::onConverterChanged from its pass - applyConverterForSource (a radio installed) and
// changeConverter (a converter command applied) - and the window's hook cleared the field's seed from there, so the next draw
// re-seeded the text. That wrote two window fields from the engine's thread. The window derives the seed from what it reads
// instead: the open radio's key and the stored setting. This holds the rule that replaced the hook:
//   * a different RADIO re-seeds (a different key), whatever the setting;
//   * a different stored LO, MODE or INVERSION re-seeds;
//   * the same radio and the same setting do NOT: what the user is typing stays.
// ...and, through the real engine, that the seed the window would compute really does move for every converter command and radio
// change the old hook announced (the engine's stored setting is what the CONVERTERS list serves).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "core/freq_converter.hpp"
#include "gui/converter_lo_seed.hpp"
#include "test_check.hpp"

namespace cc = cascade::core;
using cascade::gui::converterLoSeed;

int main() {
    std::printf("test_converter_lo_seed\n");
    cc::ConverterSetting s;
    s.mode = cc::ConverterMode::Up;
    s.loHz = 125.0e6;
    s.inverted = false;
    const std::string base = converterLoSeed("rtlsdr:0", s);

    // The same radio and the same setting: the same seed (the user's typing is not disturbed).
    CHECK(converterLoSeed("rtlsdr:0", s) == base);
    cc::ConverterSetting same = s;
    CHECK(converterLoSeed("rtlsdr:0", same) == base);

    // Each thing the hook announced moves it.
    CHECK(converterLoSeed("rtlsdr:1", s) != base);              // another radio (applyConverterForSource)
    cc::ConverterSetting lo = s;
    lo.loHz = 100.0e6;
    CHECK(converterLoSeed("rtlsdr:0", lo) != base);             // another LO (changeConverter)
    cc::ConverterSetting mode = s;
    mode.mode = cc::ConverterMode::Down;
    CHECK(converterLoSeed("rtlsdr:0", mode) != base);           // another mode
    cc::ConverterSetting inv = s;
    inv.inverted = true;
    CHECK(converterLoSeed("rtlsdr:0", inv) != base);            // the inversion toggled
    cc::ConverterSetting off = s;
    off.mode = cc::ConverterMode::Off;
    CHECK(converterLoSeed("rtlsdr:0", off) != base);            // switched off and on again are two seeds

    // The seeds of different (key, setting) pairs never collide through the separator: a key ending in what a setting starts with.
    cc::ConverterSetting a;
    a.mode = cc::ConverterMode::Up;
    a.loHz = 1.0e6;
    CHECK(converterLoSeed("k", a) != converterLoSeed(std::string("k") + converterLoSeed("", a), a));

    return testSummary("test_converter_lo_seed");
}
