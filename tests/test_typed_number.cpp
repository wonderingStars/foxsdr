// Tests for core/typed_number.hpp and the number rules of owner decision 73 (engine lane E1): what is DISPLAYED follows Windows' number format; what is TYPED is accepted in both
// forms; what a machine reads keeps the dot. This file holds the typed rule and the machine rule; the displayed rule is tests of src/slint/tests/test_slint_host_numbers.cpp, and the
// lanes' own typed parsers are in test_slint_lanel_host.cpp. Argument: the source root.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/access_number_format.hpp"
#include "core/config.hpp"
#include "core/typed_number.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::typedNumberText;

namespace {

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void testTypedForms() {
    CHECK(typedNumberText("1,5") == "1.5");
    CHECK(typedNumberText("1.5") == "1.5");
    CHECK(typedNumberText("-0,25") == "-0.25");
    CHECK(typedNumberText("145,5") == "145.5");
    CHECK(typedNumberText(",5") == ".5");
    CHECK(typedNumberText("7") == "7");
    CHECK(typedNumberText("").empty());
    // both signs, or two commas: never guessed, left for the caller's own parse to refuse or accept
    CHECK(typedNumberText("1,234.5") == "1,234.5");
    CHECK(typedNumberText("1.234,5") == "1.234,5");
    CHECK(typedNumberText("1,234,567") == "1,234,567");
    // the value read is the same either way
    CHECK(std::strtod(typedNumberText("2,75").c_str(), nullptr) == std::strtod("2.75", nullptr));
}

// must-fix lane A (owner decision 73): ONE comma and exactly three digits after it is ambiguous on a dot locale (14,200 = 14.2 or 14200) and is refused there; on a comma locale it is the decimal sign.
void testAmbiguousComma() {
    using cascade::core::typedCommaAmbiguous;
    CHECK(typedCommaAmbiguous("14,200", false) && typedCommaAmbiguous("-3,000", false) && typedCommaAmbiguous("1,500 kHz", false) && typedCommaAmbiguous(" 14,200 ", false));
    CHECK(!typedCommaAmbiguous("14,200", true) && !typedCommaAmbiguous("1,500 kHz", true));                                      // a comma locale: nothing is ambiguous
    CHECK(!typedCommaAmbiguous("14,20", false) && !typedCommaAmbiguous("14,2", false) && !typedCommaAmbiguous("14,2000", false));  // two, one and four digits after the comma
    CHECK(!typedCommaAmbiguous(",200", false) && !typedCommaAmbiguous("1,234,567", false) && !typedCommaAmbiguous("1,234.5", false) && !typedCommaAmbiguous("1.234,567", false));
    CHECK(!typedCommaAmbiguous("14.200", false) && !typedCommaAmbiguous("14200", false) && !typedCommaAmbiguous("", false) && !typedCommaAmbiguous("abc", false));
    // the two-argument reader leaves an ambiguous text alone (the caller refuses it first) and reads every other comma as before
    CHECK(typedNumberText("14,200", false) == "14,200" && typedNumberText("14,200", true) == "14.200" && typedNumberText("14,2", false) == "14.2");
    // the process-wide sign: the default is a dot, and it is what the one-argument check asks
    cascade::core::setTypedDecimalComma(false);
    CHECK(!cascade::core::typedDecimalComma() && typedCommaAmbiguous("14,200"));
    cascade::core::setTypedDecimalComma(true);
    CHECK(cascade::core::typedDecimalComma() && !typedCommaAmbiguous("14,200"));
    cascade::core::setTypedDecimalComma(false);
}

void testDisplayedSymbols() {
    // the comma locale writes a comma; the dot setting a dot; the same value
    cascade::core::access::NumberSymbols de;
    de.name = "de-DE";
    de.decimal = ",";
    de.group = ".";
    CHECK(cascade::core::access::formatNumber(1234.5, 1, de) == "1234,5");
    cascade::core::access::NumberSymbols dot;
    dot.name = "en-GB";
    CHECK(cascade::core::access::formatNumber(1234.5, 1, dot) == "1234.5");
}

// THE MACHINE RULE: the settings file keeps the dot whatever the display format is (the display format is not an input of the serializer).
void testSettingsFileKeepsTheDot() {
    cascade::core::AppConfig c;
    c.volume = 0.75;
    const std::string text = cascade::core::ConfigStore::serialize(c);
    CHECK(has(text, "0.75"));
    CHECK(!has(text, "0,75"));
}

// The display formatter is called from the window's code only; the engine and core never format a number for a person with it, so nothing a machine reads is touched.
void testNoMachineWriterUsesTheDisplayFormatter(const fs::path& root) {
    for (const char* dir : {"src/engine", "src/net", "src/dsp", "src/source", "src/sink"}) {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(root / dir, ec)) {
            if (!e.is_regular_file()) { continue; }
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp") { continue; }
            const std::string t = slurp(e.path());
            CHECK(!has(t, "formatNumber("));
            CHECK(!has(t, "setNumberLocale("));
        }
    }
    // in core only the model's own files and the speech/units readouts of the window may
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / "src/core", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("access_", 0) == 0) { continue; }
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        CHECK(!has(slurp(e.path()), "formatNumber(") || name == "typed_number.hpp" || name == "display_number.hpp");   // (must-fix lane A: the one place the window's readout units ask for the shown format, host_logic's own formatter's twin)
    }
}


// THE 33 LANGUAGES: Windows' own number symbols for each interface language (the system's table for that locale, NOT this PC's user overrides). The comma countries write a comma, and
// a number written with their symbols is read back in both forms. (Windows only: the symbols come from GetLocaleInfoEx.)
void testLanguagesFollowWindows() {
#ifdef _WIN32
    struct L { const char* name; const char* decimal; };   // expected decimal sign where it is certain; "" = only that a sign was found
    static const L langs[] = {
        {"bg-BG", ","}, {"ca-ES", ","}, {"cs-CZ", ","}, {"da-DK", ","}, {"de-DE", ","}, {"el-GR", ","}, {"es-ES", ","}, {"et-EE", ","}, {"fi-FI", ","}, {"fr-FR", ","},
        {"hr-HR", ","}, {"hu-HU", ","}, {"id-ID", ","}, {"it-IT", ","}, {"ja-JP", "."}, {"ko-KR", "."}, {"lt-LT", ","}, {"lv-LV", ","}, {"nb-NO", ","}, {"nl-NL", ","},
        {"pl-PL", ","}, {"pt-BR", ","}, {"pt-PT", ","}, {"ro-RO", ","}, {"ru-RU", ","}, {"sk-SK", ","}, {"sl-SI", ","}, {"sv-SE", ","}, {"tr-TR", ","}, {"uk-UA", ","},
        {"vi-VN", ","}, {"zh-CN", "."}, {"zh-TW", "."}, {"en-GB", "."}, {"en-US", "."}};
    int comma = 0;
    for (const L& l : langs) {
        cascade::core::access::NumberSymbols sym;
        CHECK(cascade::core::access::loadNumberSymbols(l.name, false, sym));
        CHECK(sym.decimal == l.decimal);
        const std::string shown = cascade::core::access::formatNumber(145.5, 1, sym);
        CHECK(shown == std::string("145") + l.decimal + "5");
        // typed back in either form
        CHECK(std::strtod(typedNumberText("145,5").c_str(), nullptr) == 145.5);
        CHECK(std::strtod(typedNumberText("145.5").c_str(), nullptr) == 145.5);
        if (sym.decimal == ",") { ++comma; }
    }
    CHECK(comma >= 28);   // the comma countries are the great majority of the 33
#else
    SKIP_LINUX("the regional symbols come from Windows (GetLocaleInfoEx)");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    testTypedForms();
    testAmbiguousComma();
    testDisplayedSymbols();
    testSettingsFileKeepsTheDot();
    testLanguagesFollowWindows();
    if (argc > 1) { testNoMachineWriterUsesTheDisplayFormatter(fs::path(argv[1])); } else { std::printf("usage: test_typed_number <source root>\n"); return 2; }
    return testSummary("test_typed_number");
}
