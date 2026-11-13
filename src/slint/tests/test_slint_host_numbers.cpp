// The number format of the readouts (src/slint/host_logic.cpp setNumberLocale; screen lane K, docs/multi-radio-records/screen-k.md): with no locale set the four readouts (fmtMHz, fmtKHz, spanText, cursorText)
// are today's text byte for byte; with the symbols of a comma locale they write the comma, with the sign and the digits the symbols say.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_logic.hpp"
#include "test_check.hpp"

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

}  // namespace

int main() {
    defaultsAreToday();
    commaLocale();
    setNumberLocale(nullptr);
    return testSummary("test_slint_host_numbers");
}
