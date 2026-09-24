// Tests for core/utf8_text.hpp - stepping, cutting and capitalising UTF-8 by
// hand, which the interface does in a handful of places that were written for
// ASCII.
//
// THE PROPERTY THAT MATTERS is that no operation here ever leaves HALF a
// character behind. A buffer sized for an English sentence and handed a
// Portuguese one is cut short; cut at a byte, the last letter becomes a lone
// lead byte, which ImGui draws as a box - on the screen of exactly the person
// the translation was made for. So every cut is checked for well-formed
// output, at every length, not only at the lengths a hand-picked case lands on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/utf8_text.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "test_check.hpp"

namespace {

using cascade::core::copyVisibleUtf8;
using cascade::core::formatUtf8;
using cascade::core::upperLegend;
using cascade::core::utf8CharLen;
using cascade::core::utf8Count;
using cascade::core::utf8Floor;

// Strictly well-formed: every lead byte followed by exactly its continuation
// bytes, and no continuation byte where a character should start.
bool wellFormed(const char* s) {
    const std::size_t n = std::strlen(s);
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c < 0xE0) {
            len = 2;
        } else if (c >= 0xE0 && c < 0xF0) {
            len = 3;
        } else if (c >= 0xF0 && c < 0xF5) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) { return false; }
        }
        i += len;
    }
    return true;
}

// Letters of two, two and three bytes, and a four-byte one, so every cut
// position lands inside a character of each length somewhere.
const char* const kMixed = "Wy\xC5\x82\xC4\x85" "cz \xE2\x80\x94 \xF0\x9F\x93\xBB r\xC3\xA1" "dio";

void testCharLen() {
    std::printf("  utf8CharLen: 1/2/3/4 bytes, and 1 for anything broken\n");
    CHECK(utf8CharLen("a") == 1);
    CHECK(utf8CharLen("\xC5\x82") == 2);           // l with stroke
    CHECK(utf8CharLen("\xE2\x80\x94") == 3);       // em dash
    CHECK(utf8CharLen("\xF0\x9F\x93\xBB") == 4);   // radio
    CHECK(utf8CharLen("\x82x") == 1);              // stray continuation byte
    CHECK(utf8CharLen("\xC5") == 1);               // lead byte then the NUL
    CHECK(utf8CharLen("\xE2\x80") == 1);           // three-byte lead cut short
    CHECK(utf8Count(kMixed) == 16);
    CHECK(utf8Count("") == 0);
    CHECK(utf8Count(nullptr) == 0);
}

void testFloor() {
    std::printf("  utf8Floor: every prefix of a mixed string cut back to a whole character\n");
    const std::size_t n = std::strlen(kMixed);
    for (std::size_t cut = 0; cut <= n; ++cut) {
        const std::size_t keep = utf8Floor(kMixed, cut);
        CHECK(keep <= cut);
        CHECK(cut - keep <= 3);  // never more than one character given back
        const std::string prefix(kMixed, keep);
        if (!wellFormed(prefix.c_str())) {
            std::printf("      cut %zu kept %zu: not well formed\n", cut, keep);
        }
        CHECK(wellFormed(prefix.c_str()));
    }
    CHECK(utf8Floor("abc", 3) == 3);  // ASCII is never touched
    CHECK(utf8Floor("ab\xC3\xA9", 3) == 2);
    CHECK(utf8Floor("ab\xC3\xA9", 4) == 4);
    CHECK(utf8Floor(nullptr, 5) == 0);
}

void testFormat() {
    std::printf("  formatUtf8: a sentence too long for its buffer is cut on a character\n");
    // Every buffer size from 1 to past the whole sentence.
    const char* word = "\xC3\x89tat du r\xC3\xA9" "cepteur : arr\xC3\xAAt\xC3\xA9";
    const std::size_t whole = std::strlen(word);
    for (std::size_t cap = 1; cap <= whole + 2; ++cap) {
        char buf[64];
        std::memset(buf, 'X', sizeof(buf));
        const int r = formatUtf8(buf, cap, "%s", word);
        CHECK(r == static_cast<int>(whole));  // snprintf's own answer
        CHECK(std::strlen(buf) < cap);
        if (!wellFormed(buf)) { std::printf("      cap %zu: \"%s\" is cut mid-character\n", cap, buf); }
        CHECK(wellFormed(buf));
        // And nothing is lost that fitted: at most one character given back.
        CHECK(std::strlen(buf) + 3 >= std::min(cap - 1, whole));
    }
    // English that fits is exactly snprintf's output.
    char en[32];
    CHECK(formatUtf8(en, sizeof(en), "%d ON", 3) == 4);
    CHECK(std::strcmp(en, "3 ON") == 0);
    // A zero-sized buffer is not written at all, and the answer is snprintf's
    // measurement - which is how a caller sizes a buffer before filling it.
    char zero[1] = {'Q'};
    CHECK(formatUtf8(zero, 0, "%s", "xyz") == 3);
    CHECK(zero[0] == 'Q');
    CHECK(formatUtf8(nullptr, 0, "%d-%d", 10, 20) == 5);
}

void testCopyVisible() {
    std::printf("  copyVisibleUtf8: stops at ##, never splits a character\n");
    char buf[96];
    CHECK(copyVisibleUtf8(buf, sizeof(buf), "Plugins###plugins") == 7);
    CHECK(std::strcmp(buf, "Plugins") == 0);
    CHECK(copyVisibleUtf8(buf, sizeof(buf), "##only") == 0);
    CHECK(buf[0] == '\0');
    const char* label = "R\xC3\xA1" "di\xC3\xB6###radio";
    for (std::size_t cap = 1; cap <= 12; ++cap) {
        char small[16];
        copyVisibleUtf8(small, cap, label);
        if (!wellFormed(small)) { std::printf("      cap %zu: cut mid-character\n", cap); }
        CHECK(wellFormed(small));
        CHECK(std::strstr(small, "#") == nullptr);
    }
}

void testUpper() {
    std::printf("  upperLegend: Latin-1 and Latin Extended-A capitals, ASCII unchanged\n");
    CHECK(upperLegend("Satellites MAP") == "SATELLITES MAP");
    CHECK(upperLegend("Portugu\xC3\xAAs (Brasil)") == "PORTUGU\xC3\x8AS (BRASIL)");
    CHECK(upperLegend("wy\xC5\x82\xC4\x85" "cz") == "WY\xC5\x81\xC4\x84" "CZ");  // l-stroke, a-ogonek
    CHECK(upperLegend("\xC5\xBC\xC3\xB3\xC5\x82w") == "\xC5\xBB\xC3\x93\xC5\x81W");  // zolw
    CHECK(upperLegend("stra\xC3\x9F" "e") == "STRA\xC3\x9F" "E");  // sharp s has no simple capital
    CHECK(upperLegend("\xC3\xB7") == "\xC3\xB7");  // the division sign is not a letter
    CHECK(upperLegend("ALREADY") == "ALREADY");
    CHECK(upperLegend("") == "");
}

}  // namespace

int main() {
    testCharLen();
    testFloor();
    testFormat();
    testCopyVisible();
    testUpper();
    return testSummary("test_utf8_text");
}
