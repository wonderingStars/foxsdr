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
#include <random>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif

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
    // The boundary byte on each side of every restricted second byte (Unicode table 3-7, what Rust's from_utf8 accepts).
    CHECK(!isValidUtf8("\xE0\x9F\x80") && isValidUtf8("\xE0\xA0\x80"));
    CHECK(isValidUtf8("\xED\x9F\xBF") && !isValidUtf8("\xED\xA0\x80"));
    CHECK(!isValidUtf8("\xF0\x8F\xBF\xBF") && isValidUtf8("\xF0\x90\x80\x80"));
    CHECK(isValidUtf8("\xF4\x8F\xBF\xBF") && !isValidUtf8("\xF4\x90\x80\x80"));
    CHECK(!isValidUtf8("\xC1\x80") && isValidUtf8("\xC2\x80"));
    CHECK(!isValidUtf8("\xF5\x80\x80\x80") && !isValidUtf8("\xF8\x88\x80\x80\x80"));
    // A four-byte sequence cut after 1, 2 and 3 bytes.
    CHECK(utf8ValidUpTo("ab\xF0") == 2 && utf8ValidUpTo("ab\xF0\x9F") == 2 && utf8ValidUpTo("ab\xF0\x9F\x93") == 2);
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
        for (int b = 0; b < 256; ++b) {
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
        if (line.find("service said: AirSpy M") != std::string::npos && line.find("INI v1.0") != std::string::npos) { sawService = true; }
    }
    CHECK(sawService);
}

void testAnsiFirst() {
    std::printf("  ANSI first: the bytes that are not UTF-8 are read in the code page, valid UTF-8 around them is kept\n");
#if defined(_WIN32)
    // What the STL's / Windows' own sentence looks like in a Windows-1252 machine's answer.
    const std::string sp = "El sistema no puede encontrar el archivo especificado. (c\xF3" "digo 2)";
    CHECK(repairUtf8WithCodePage(sp, 1252) == "El sistema no puede encontrar el archivo especificado. (c\xC3\xB3" "digo 2)");
    CHECK(repairUtf8WithCodePage("Impossible de trouver le fichier sp\xE9" "cifi\xE9", 1252) == "Impossible de trouver le fichier sp\xC3\xA9" "cifi\xC3\xA9");
    CHECK(repairUtf8WithCodePage("Der Pfad f\xFCr \xDF", 1252) == "Der Pfad f\xC3\xBCr \xC3\x9F");
    // Mixed: the valid UTF-8 letters stay, the lone ANSI byte is converted.
    CHECK(repairUtf8WithCodePage("Portugu\xC3\xAAs c\xF3" "digo", 1252) == "Portugu\xC3\xAAs c\xC3\xB3" "digo");
    CHECK(repairUtf8WithCodePage("plain", 1252) == "plain");
    CHECK(isValidUtf8(repairUtf8WithCodePage(std::string(300, '\xF3'), 1252)));
    // Double-byte code pages: each run is converted whole, and a lead byte takes its trail byte even when that is ASCII
    // (0x7B here is '{' and is valid UTF-8 on its own).
    CHECK(repairUtf8WithCodePage("\x93\xFA\x96\x7B\x8C\xEA", 932) == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");   // Shift-JIS, three kanji
    CHECK(repairUtf8WithCodePage("A\x93\xFA\x96\x7B\x8C\xEA" "B", 932) == "A\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "B");
    CHECK(repairUtf8WithCodePage("\xD6\xD0\xCE\xC4", 936) == "\xE4\xB8\xAD\xE6\x96\x87");                         // GBK, two hanzi
    CHECK(repairUtf8WithCodePage("ab\x93", 932) == "ab\xEF\xBF\xBD");   // a lone lead byte at the end: refused, U+FFFD
    CHECK(codePageToUtf8("\x93\xFA\x96\x7B", 932) == "\xE6\x97\xA5\xE6\x9C\xAC");
    // Code page 1251 inside text, and 65001 (nothing to read: U+FFFD).
    CHECK(repairUtf8WithCodePage("x \xCF\xF0\xE8\xE2\xE5\xF2 y", 1251) == "x \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 y");
    CHECK(repairUtf8WithCodePage(kBad8, 65001) == sanitizeUtf8(kBad8));
    CHECK(repairUtf8WithCodePage("abc\xC3", 65001) == "abc\xEF\xBF\xBD");
    CHECK(codePageToUtf8(kBad17, 65001) == sanitizeUtf8(kBad17));
    if (::GetACP() == 1252) {
        CHECK(utf8OrAnsiToUtf8(sp) == repairUtf8WithCodePage(sp, 1252));
        diagLogf("ansi-first: %s", sp.c_str());
        bool saw = false;
        for (const std::string& l : DiagLog::instance().ringSnapshot()) {
            if (l.find("ansi-first: El sistema no puede encontrar el archivo especificado. (c\xC3\xB3" "digo 2)") != std::string::npos) { saw = true; }
        }
        CHECK(saw);
    } else {
        std::printf("    (this machine's ANSI code page is %u, not 1252: the log-ring letter check is skipped)\n", ::GetACP());
    }
#else
    SKIP_LINUX("code pages are a Windows facility");
#endif
}

// DOUBLE-BYTE PAGES (review 2026-11-23, finding 4): a Chinese, Japanese, Korean machine's own sentence is read in its page even when some of
// its two-byte letters look like valid UTF-8 (CF B5 CD B3 is the Chinese word xitong in GBK and two Greek-looking letters in UTF-8), and real
// UTF-8 with one damaged byte keeps its valid letters on both sides of the damage.
void testDoubleBytePages() {
    std::printf("  double-byte code pages: whole sentences, GBK letters that look like UTF-8, valid UTF-8 around one bad byte, a fuzz of 100000 byte strings\n");
#if defined(_WIN32)
    CHECK(isDoubleByteCodePage(936) && isDoubleByteCodePage(932) && isDoubleByteCodePage(949) && isDoubleByteCodePage(950));
    CHECK(!isDoubleByteCodePage(1252) && !isDoubleByteCodePage(1251) && !isDoubleByteCodePage(65001));
    CHECK(isDbcsLeadByte(936, 0x81) && !isDbcsLeadByte(936, 0x41) && !isDbcsLeadByte(1252, 0x81));
    struct Case { unsigned cp; const char* in; const char* out; const char* what; };
    const Case cases[] = {
        {936, "\xCF\xB5\xCD\xB3\xB4\xED\xCE\xF3", "\xE7\xB3\xBB\xE7\xBB\x9F\xE9\x94\x99\xE8\xAF\xAF", "cp936: the sentence that starts with a pair that is valid UTF-8"},
        {936, "\xD5\xD2\xB2\xBB\xB5\xBD\xD6\xB8\xB6\xA8\xB5\xC4\xCE\xC4\xBC\xFE", "\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0\xE6\x8C\x87\xE5\xAE\x9A\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6", "cp936: a whole sentence"},
        {936, "\xD6\xD0\xCE\xC4", "\xE4\xB8\xAD\xE6\x96\x87", "cp936: two letters"},
        {950, "\xA8\x74\xB2\xCE\xBF\xF9\xBB\x7E", "\xE7\xB3\xBB\xE7\xB5\xB1\xE9\x8C\xAF\xE8\xAA\xA4", "cp950: a sentence"},
        {950, "\xA7\xE4\xA4\xA3\xA8\xEC\xAB\xFC\xA9\x77\xAA\xBA\xC0\xC9\xAE\xD7", "\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0\xE6\x8C\x87\xE5\xAE\x9A\xE7\x9A\x84\xE6\xAA\x94\xE6\xA1\x88", "cp950: a longer sentence"},
        {932, "\x83\x56\x83\x58\x83\x65\x83\x80\x83\x47\x83\x89\x81\x5B", "\xE3\x82\xB7\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xA0\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC", "cp932: katakana with an ASCII trail byte (5B)"},
        {932, "\x8E\x77\x92\xE8\x82\xB3\x82\xEA\x82\xBD\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xAA\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82\xB9\x82\xF1",
         "\xE6\x8C\x87\xE5\xAE\x9A\xE3\x81\x95\xE3\x82\x8C\xE3\x81\x9F\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB\xE3\x81\x8C\xE8\xA6\x8B\xE3\x81\xA4\xE3\x81\x8B\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93", "cp932: a whole sentence"},
        {949, "\xBD\xC3\xBD\xBA\xC5\xDB\x20\xBF\xC0\xB7\xF9", "\xEC\x8B\x9C\xEC\x8A\xA4\xED\x85\x9C\x20\xEC\x98\xA4\xEB\xA5\x98", "cp949: Hangul with a space"},
        {949, "\xC1\xF6\xC1\xA4\xB5\xC8\x20\xC6\xC4\xC0\xCF\xC0\xBB\x20\xC3\xA3\xC0\xBB\x20\xBC\xF6\x20\xBE\xF8\xBD\xC0\xB4\xCF\xB4\xD9",
         "\xEC\xA7\x80\xEC\xA0\x95\xEB\x90\x9C\x20\xED\x8C\x8C\xEC\x9D\xBC\xEC\x9D\x84\x20\xEC\xB0\xBE\xEC\x9D\x84\x20\xEC\x88\x98\x20\xEC\x97\x86\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4", "cp949: a whole sentence"},
        {1251, "x \xCF\xF0\xE8\xE2\xE5\xF2 y", "x \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 y", "cp1251: Cyrillic inside ASCII"},
        {1252, "c\xF3" "digo", "c\xC3\xB3" "digo", "cp1252: o acute"},
    };
    for (const Case& c : cases) {
        const std::string got = repairUtf8WithCodePage(c.in, c.cp);
        if (got != c.out) { std::printf("FAIL %s\n", c.what); }
        CHECK(got == c.out);
        CHECK(isValidUtf8(got));
    }
    // Valid UTF-8, ONE bad byte, valid UTF-8: both sides stay as they are, in every page (the bad byte is a lead byte of the double-byte pages).
    const std::string left = "\xE4\xB8\xAD";
    const std::string right = "\xE6\x96\x87";
    for (const unsigned cp : {932u, 936u, 949u, 950u, 1251u, 1252u, 65001u}) {
        for (const char* bad : {"\x81", "\xFF", "\x93", "\xC0"}) {
            const std::string got = repairUtf8WithCodePage(left + bad + right, cp);
            const bool kept = got.compare(0, left.size(), left) == 0 && got.size() >= right.size() && got.compare(got.size() - right.size(), right.size(), right) == 0;
            if (!kept) { std::printf("FAIL cp%u bad byte %02x: both sides of the bad byte must stay\n", cp, static_cast<unsigned>(static_cast<unsigned char>(bad[0]))); }
            CHECK(kept);
            CHECK(isValidUtf8(got));
        }
        const std::string text = "ab " + left + " cd\x81" + right + " ef";   // the same with ASCII around it
        const std::string got = repairUtf8WithCodePage(text, cp);
        CHECK(got.find(left) != std::string::npos && got.find(right) != std::string::npos && isValidUtf8(got));
    }
    // A fuzz with a fixed seed: whatever it is given, the repair returns valid UTF-8 and does not crash.
    std::mt19937 rng(20261123u);
    const unsigned pages[] = {1252u, 1251u, 932u, 936u, 949u, 950u, 65001u, 0u};
    std::size_t bad = 0;
    for (int i = 0; i < 100000; ++i) {
        std::string s(rng() % 65u, '\0');
        const unsigned mode = static_cast<unsigned>(i) % 3u;
        for (char& ch : s) {
            const unsigned r = rng();
            unsigned char b = static_cast<unsigned char>(r >> 8);
            if (mode == 1u && (r & 7u) != 0u) { b = static_cast<unsigned char>(0x20 + (b % 0x5F)); }      // ASCII with sparse high bytes
            if (mode == 2u && (r & 3u) != 0u) { b = static_cast<unsigned char>(0x80 + (b % 0x80)); }      // mostly high bytes
            ch = static_cast<char>(b);
        }
        const unsigned cp = pages[static_cast<unsigned>(i) % 8u];
        if (!isValidUtf8(repairUtf8WithCodePage(s, cp)) || !isValidUtf8(codePageToUtf8(s, cp))) { ++bad; }
    }
    CHECK(bad == 0);
#else
    SKIP_LINUX("code pages are a Windows facility");
#endif
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
    testAnsiFirst();
    testDoubleBytePages();
    return testSummary("test_utf8_sanitize");
}
