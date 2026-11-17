// The number format of the readouts (src/slint/host_logic.cpp setNumberLocale; screen lane K, docs/multi-radio-records/screen-k.md): with no locale set the four readouts (fmtMHz, fmtKHz, spanText, cursorText)
// are today's text byte for byte; with the symbols of a comma locale they write the comma, with the sign and the digits the symbols say.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/display_number.hpp"
#include "core/typed_number.hpp"
#include "host_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <string>

using namespace foxsdr::slinthost;
namespace acc = cascade::core::access;

namespace {

void defaultsAreToday() {
    setNumberLocale(nullptr);
    CHECK(fmtMHz(145212500.0) == "145.212500 MHz");
    CHECK(fmtKHz(12345.0) == "12.3 kHz");
    CHECK(spanText(2.4e6) == "SPAN 2.400 MHz");
    CHECK(spanText(2.4e9) == "SPAN 2.400 GHz");
    CHECK(spanText(48e3) == "SPAN 48.0 kHz");
    CHECK(spanText(500.0) == "SPAN 500 Hz");
    CHECK(cursorText(145.2124e6, -78.4).find("145.212 MHz") == 0);
    CHECK(cursorText(145.2124e6, -78.4).find("-78.4 dB") != std::string::npos);
}

void commaLocale() {
    acc::NumberSymbols de;
    de.name = "de-DE";
    de.decimal = ",";
    de.group = ".";
    setNumberLocale(&de);
    CHECK(fmtMHz(145212500.0) == "145,212500 MHz");
    CHECK(fmtKHz(12345.0) == "12,3 kHz");
    CHECK(spanText(2.4e6) == "SPAN 2,400 MHz");
    CHECK(spanText(500.0) == "SPAN 500 Hz");
    CHECK(cursorText(145.2124e6, -78.4).find("145,212 MHz") == 0);
    CHECK(cursorText(145.2124e6, -78.4).find("-78,4 dB") != std::string::npos);
    CHECK(fmtMHz(-1.5e6) == "-1,500000 MHz");                       // a negative keeps its sign
    // a locale with an Arabic-Indic digit set and the digit policy that asks for it
    acc::NumberSymbols ar;
    ar.name = "ar-SA";
    ar.decimal = "\xD9\xAB";   // U+066B ARABIC DECIMAL SEPARATOR
    ar.digitSubstitution = 2;
    for (int i = 0; i < 10; ++i) { ar.nativeDigits[static_cast<std::size_t>(i)] = std::string("\xD9") + static_cast<char>(0xA0 + i); }   // U+0660..U+0669
    setNumberLocale(&ar, acc::DigitPolicy::Native);
    const std::string native = fmtMHz(7074000.0);
    CHECK(native.find("\xD9\xA7") != std::string::npos);            // the digit seven
    CHECK(native.find("7") == std::string::npos);
    setNumberLocale(&ar, acc::DigitPolicy::Latin);                   // the Latin policy keeps 0-9 but still uses the locale's decimal sign
    CHECK(fmtMHz(7074000.0).find("7\xD9\xAB") != std::string::npos);
    // switching it off again gives today's text
    setNumberLocale(nullptr);
    CHECK(fmtMHz(7074000.0) == "7.074000 MHz");
}

// ENGINE LANE E1 (owner decision 73): the other readouts follow the same setting. With nothing set they are today's text; with a comma locale they write the comma.
void otherReadouts() {
    setNumberLocale(nullptr);
    CHECK(fmtNum(1.5, 1) == "1.5");
    CHECK(fmtNum(-1.5, 1) == "-1.5");
    CHECK(fmtNum(2.5, 1, true) == "+2.5");
    CHECK(fmtNum(-2.5, 1, true) == "-2.5");
    CHECK(fmtNum(0.0, 2, true) == "+0.00");
    CHECK(fmtNum(7.0, 0) == "7");
    CHECK(fmtNum(145.5, 6) == "145.500000");
    CHECK(fmtRate(2.4e6) == "2.400 MS/s");
    CHECK(fmtRate(0.0) == "--");
    CHECK(levelText(-38.4, true) == "-38.4 dB");
    CHECK(localizeNumber("output lines, 2.5 s mean", 2.5, 1) == "output lines, 2.5 s mean");   // no locale: unchanged
    acc::NumberSymbols de;
    de.name = "de-DE";
    de.decimal = ",";
    de.group = ".";
    setNumberLocale(&de);
    CHECK(fmtNum(1.5, 1) == "1,5");
    CHECK(fmtNum(-1.5, 1) == "-1,5");
    CHECK(fmtNum(2.5, 1, true) == "+2,5");
    CHECK(fmtNum(7.0, 0) == "7");
    CHECK(fmtNum(0.75, 2) == "0,75");
    CHECK(fmtNum(145.5, 6) == "145,500000");
    CHECK(fmtRate(2.4e6) == "2,400 MS/s");
    CHECK(fmtRate(0.0) == "--");
    CHECK(levelText(-38.4, true) == "-38,4 dB");
    CHECK(levelText(-80.0, true) == "-60 dB");                                                  // the floor stays as it was
    CHECK(localizeNumber("output lines, 2.5 s mean", 2.5, 1) == "output lines, 2,5 s mean");
    CHECK(localizeNumber("nothing of it here", 2.5, 1) == "nothing of it here");
    // a language whose translated sentence puts the number elsewhere is still localised (the number is found, not the position)
    CHECK(localizeNumber("Ausgabezeilen, Mittel 2.5 s", 2.5, 1) == "Ausgabezeilen, Mittel 2,5 s");
    setNumberLocale(nullptr);
    CHECK(fmtNum(0.75, 2) == "0.75");
}

// must-fix lane A (owner decision 73): what a typed comma means. On a computer whose decimal sign is a dot, ONE comma followed by exactly three digits ("14,200") is ambiguous and refused with
// a sentence; on a comma locale the comma is the decimal sign. The other shapes are read either way.
void typedComma() {
    double hz = 0.0;
    bool amb = false;
    setNumberLocale(nullptr);   // a dot locale (the default)
    CHECK(!cascade::core::typedDecimalComma());
    CHECK(!parseFrequencyHz("14,200", hz, &amb) && amb);
    amb = false;
    CHECK(!parseFrequencyHz("14,200 kHz", hz, &amb) && amb);
    amb = false;
    CHECK(parseFrequencyHz("14,2", hz, &amb) && !amb && std::fabs(hz - 14.2e6) < 1.0);
    CHECK(parseFrequencyHz("14,2000", hz, &amb) && !amb && std::fabs(hz - 14.2e6) < 1.0);
    CHECK(parseFrequencyHz("14.200", hz, &amb) && !amb && std::fabs(hz - 14.2e6) < 1.0);
    CHECK(parseFrequencyHz("145,5 MHz", hz, &amb) && !amb && std::fabs(hz - 145.5e6) < 1.0);
    CHECK(parseFrequencyHz("7074", hz, &amb) && !amb && std::fabs(hz - 7074.0e6) < 1.0);
    CHECK(!parseFrequencyHz("14,200", hz));   // the two-argument form refuses it too
    CHECK(cascade::core::typedCommaAmbiguous("14,200") && cascade::core::typedCommaAmbiguous("-3,000 kHz") && cascade::core::typedCommaAmbiguous(" 1,500"));
    CHECK(!cascade::core::typedCommaAmbiguous("14,20") && !cascade::core::typedCommaAmbiguous("14,2000") && !cascade::core::typedCommaAmbiguous(",200") && !cascade::core::typedCommaAmbiguous("1,234,567") &&
          !cascade::core::typedCommaAmbiguous("1,234.5") && !cascade::core::typedCommaAmbiguous("14.200") && !cascade::core::typedCommaAmbiguous("14200") && !cascade::core::typedCommaAmbiguous(""));
    CHECK(!typedCommaSentence().empty());
    // a comma locale: the comma is the decimal sign and nothing is ambiguous
    acc::NumberSymbols de;
    de.name = "de-DE";
    de.decimal = ",";
    de.group = ".";
    setNumberLocale(&de);
    CHECK(cascade::core::typedDecimalComma());
    amb = false;
    CHECK(parseFrequencyHz("14,200", hz, &amb) && !amb && std::fabs(hz - 14.2e6) < 1.0);
    CHECK(parseFrequencyHz("14,200 MHz", hz, &amb) && !amb && std::fabs(hz - 14.2e6) < 1.0);
    CHECK(!cascade::core::typedCommaAmbiguous("14,200"));
    // the format the SIGNAL page's readouts follow is the same one (core/display_number.hpp)
    CHECK(cascade::core::localizeDisplayNumber("on 2.5 s", 2.5, 1) == "on 2,5 s" && cascade::core::displayNumber(0.75, 2) == "0,75");
    setNumberLocale(nullptr);
    CHECK(!cascade::core::typedDecimalComma());
    CHECK(cascade::core::localizeDisplayNumber("on 2.5 s", 2.5, 1) == "on 2.5 s" && cascade::core::displayNumber(0.75, 2) == "0.75");
    // an Accessibility "always a dot" choice is a dot locale even on a comma computer: no symbols set
    CHECK(!cascade::core::typedDecimalComma());
}

}  // namespace

int main() {
    defaultsAreToday();
    commaLocale();
    otherReadouts();
    typedComma();
    setNumberLocale(nullptr);
    return testSummary("test_slint_host_numbers");
}
