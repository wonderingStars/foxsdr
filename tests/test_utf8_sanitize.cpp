// Tests for the UTF-8 repair helpers in core/utf8_text.hpp and the log ring's use of them.
//
// WHY (0.99.112 field crash groups 6CCFD0C0E58FC30C and 104F425230C2A742): the
// new window's slint::SharedString aborts the process on a text that is not
// valid UTF-8 ("Utf8Error { valid_up_to: 8, error_len: Some(1) }"). Text the
// program does not write - a device's answer, a localised Windows sentence in
// the ANSI code page - reaches it through the log ring and the publishers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"
#include "core/utf8_text.hpp"

#include <cstdio>
#include <string>

#include "test_check.hpp"

namespace {

using namespace cascade::core;

// Windows-1252 0xF3 (o acute) inside ASCII, the bad byte at offset 8 and at 17:
// the two offsets in the field reports.
const std::string kBad8 = std::string("AirSpy M") + "\xF3" + "INI v1.0";
const std::string kBad17 = std::string("localised service") + "\xF3" + " text";

void testValidity() {
    std::printf("  isValidUtf8 / utf8ValidUpTo: the field offsets, and the edges\n");
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("plain ASCII 123"));
    CHECK(isValidUtf8("Portugu\xC3\xAAs, \xD0\xA0\xD1\x83\xD1\x81, \xE4\xB8\xAD\xE5\x9B\xBD, \xF0\x9F\x93\xBB"));
    CHECK(!isValidUtf8(kBad8));
    CHECK(utf8ValidUpTo(kBad8) == 8);   // Utf8Error { valid_up_to: 8 }
    CHECK(utf8ValidUpTo(kBad17) == 17);  // Utf8Error { valid_up_to: 17 }
    CHECK(!isValidUtf8("abc\xC3"));            // truncated sequence at the end
    CHECK(utf8ValidUpTo("abc\xC3") == 3);
    CHECK(!isValidUtf8("a\xC0\xAF" "b"));      // overlong '/'
    CHECK(!isValidUtf8("a\xE0\x80\x80" "b"));  // overlong NUL, 3 bytes
    CHECK(!isValidUtf8("\xED\xA0\x80"));       // a surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80"));   // above U+10FFFF
    CHECK(!isValidUtf8("\xFF"));
    CHECK(!isValidUtf8("\x80"));               // a stray continuation byte
    CHECK(isValidUtf8(std::string("a\0b", 3)));  // an embedded NUL is valid UTF-8
}

void testSanitize() {
    std::printf("  sanitizeUtf8: invalid bytes become U+FFFD, valid text is untouched\n");
    CHECK(sanitizeUtf8("") == "");
    CHECK(sanitizeUtf8("plain") == "plain");
    const std::string multi = "Portugu\xC3\xAAs \xE4\xB8\xAD";
    CHECK(sanitizeUtf8(multi) == multi);
    Utf8Repair r;
    CHECK(sanitizeUtf8(kBad8, &r) == "AirSpy M\xEF\xBF\xBDINI v1.0");
    CHECK(r.firstBad == 8 && r.badBytes == 1);
    CHECK(sanitizeUtf8(kBad17, &r) == "localised service\xEF\xBF\xBD text");
    CHECK(r.firstBad == 17);
    CHECK(sanitizeUtf8("abc\xC3") == "abc\xEF\xBF\xBD");                // truncated at the end
    CHECK(sanitizeUtf8("a\xC0\xAF" "b") == "a\xEF\xBF\xBD\xEF\xBF\xBD" "b");  // overlong: one per byte
    CHECK(sanitizeUtf8("\xFF\xFE") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    const std::string nul("a\0b", 3);
    CHECK(sanitizeUtf8(nul) == nul);
    const std::string nulBad("a\0\xF3" "b", 4);
    CHECK(sanitizeUtf8(nulBad) == std::string("a\0\xEF\xBF\xBD" "b", 6));
    // Whatever goes in, what comes out is valid (every single byte, and every pair).
    for (int a = 0; a < 256; ++a) {
        const std::string one(1, static_cast<char>(a));
        CHECK(isValidUtf8(sanitizeUtf8(one)));
        for (int b = 0; b < 256; b += 7) {
            std::string two = one;
            two.push_back(static_cast<char>(b));
            CHECK(isValidUtf8(sanitizeUtf8(two)));
        }
    }
}

void testInPlace() {
    std::printf("  utf8ReplaceInvalidInPlace: same length, '?' for each bad byte\n");
    std::string a = kBad8;
    CHECK(utf8ReplaceInvalidInPlace(a.data(), a.size()) == 1);
    CHECK(a == "AirSpy M?INI v1.0");
    std::string ok = "Portugu\xC3\xAAs";
    CHECK(utf8ReplaceInvalidInPlace(ok.data(), ok.size()) == 0);
    CHECK(ok == "Portugu\xC3\xAAs");
    CHECK(utf8ReplaceInvalidInPlace(nullptr, 0) == 0);
}

void testHex() {
    std::printf("  hexOfBytes: at most 32 bytes from the offset\n");
    CHECK(hexOfBytes(kBad8, 8, 3) == "f3 49 4e");
    CHECK(hexOfBytes("", 0) == "");
    CHECK(hexOfBytes(std::string(100, 'A'), 90).size() == 10 * 3 - 1);
    CHECK(hexOfBytes(std::string(100, 'A'), 0).size() == 32 * 3 - 1);
}

void testCleanDeviceText() {
    std::printf("  cleanDeviceText: a counted field that was never terminated\n");
    // The real text, then the junk a firmware leaves after it.
    const std::string field = std::string("AirSpy MINI v1.0.0-rc10-6-g1234") + '\0' + "\xF3\xFF junk";
    CHECK(cleanDeviceText(field) == "AirSpy MINI v1.0.0-rc10-6-g1234");
    CHECK(cleanDeviceText("  v1\t\r\n ") == "v1");
    CHECK(cleanDeviceText(std::string("\0abc", 4)) == "");
    CHECK(cleanDeviceText(kBad8) == "AirSpy M\xEF\xBF\xBDINI v1.0");
    CHECK(isValidUtf8(cleanDeviceText(std::string(127, '\xF3'))));
}

void testAnsi() {
    std::printf("  codePageToUtf8: Windows-1252 and 1251 to UTF-8\n");
#if defined(_WIN32)
    CHECK(codePageToUtf8("Controlador de host USB \xF3", 1252) == "Controlador de host USB \xC3\xB3");
    CHECK(codePageToUtf8("caf\xE9 \x80", 1252) == "caf\xC3\xA9 \xE2\x82\xAC");  // e acute, euro sign
    CHECK(codePageToUtf8("\xCF\xF0\xE8\xE2\xE5\xF2", 1251) == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");  // Cyrillic
    CHECK(codePageToUtf8("", 1252) == "");
    CHECK(codePageToUtf8("ASCII only", 1252) == "ASCII only");
    CHECK(isValidUtf8(codePageToUtf8(std::string(256, '\xA5'), 1252)));
#else
    SKIP_LINUX("code pages are a Windows facility");
#endif
    std::printf("  utf8OrAnsiToUtf8: valid UTF-8 is kept, anything else is ANSI\n");
    CHECK(utf8OrAnsiToUtf8("Portugu\xC3\xAAs") == "Portugu\xC3\xAAs");
    CHECK(utf8OrAnsiToUtf8("plain") == "plain");
    CHECK(isValidUtf8(utf8OrAnsiToUtf8(kBad8)));
    CHECK(isValidUtf8(utf8OrAnsiToUtf8(kBad17)));
}

void testLogRing() {
    std::printf("  DiagLog: a line carrying a bad byte, or cut half way through a letter, is valid in the ring\n");
    DiagLog& log = DiagLog::instance();
    diagLogf("service said: %s", kBad8.c_str());
    diagLogf("%s", kBad17.c_str());
    // A long line whose 191-byte cut would land inside a two-byte letter.
    std::string longLine(170, 'x');
    for (int i = 0; i < 20; ++i) { longLine += "\xC3\xA9"; }
    diagLogf("%s", longLine.c_str());
    bool sawService = false;
    for (const std::string& line : log.ringSnapshot()) {
        CHECK(isValidUtf8(line));
        if (line.find("service said: AirSpy M?INI") != std::string::npos) { sawService = true; }
    }
    CHECK(sawService);
}

}  // namespace

int main() {
    testValidity();
    testSanitize();
    testInPlace();
    testHex();
    testCleanDeviceText();
    testAnsi();
    testLogRing();
    return testSummary("test_utf8_sanitize");
}
