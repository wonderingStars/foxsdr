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
using cascade::core::formatText;
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

// THE STRING FORM IS NEVER CUT. A translated sentence takes two or three bytes
// a letter in Russian, Greek or Chinese, so a line that fitted its English
// buffer comfortably lost its end on exactly those screens. The std::string
// overloads size the result to the text, so the check is the whole sentence,
// byte for byte, with every kind of conversion in it - and a length well past
// any buffer the interface ever sized for English.
void testFormatWhole() {
    std::printf("  formatUtf8(std::string&) / formatText: a long translation arrives whole\n");
    const char* ru =
        "Воздушное судно %s не передавало своё положение уже %.1f с, а всего за "
        "последний час от него было принято %d сообщений; если так будет "
        "продолжаться, отметка будет удалена с карты, и её след исчезнет вместе с "
        "ней, поэтому проверьте антенну, кабель и выбранную частоту приёмника.";
    const std::string want =
        "Воздушное судно G-ABCD не передавало своё положение уже 12.5 с, а всего за "
        "последний час от него было принято 42 сообщений; если так будет "
        "продолжаться, отметка будет удалена с карты, и её след исчезнет вместе с "
        "ней, поэтому проверьте антенну, кабель и выбранную частоту приёмника.";
    CHECK(want.size() > 400);  // the premise: longer than any fixed buffer it replaces
    std::string got = "stale text that must be replaced";
    const int r = formatUtf8(got, ru, "G-ABCD", 12.5, 42);
    if (got != want) {
        std::printf("      got %zu bytes, want %zu: \"%s\"\n", got.size(), want.size(), got.c_str());
    }
    CHECK(got == want);
    CHECK(r == static_cast<int>(want.size()));
    CHECK(wellFormed(got.c_str()));
    CHECK(formatText(ru, "G-ABCD", 12.5, 42) == want);

    // Chinese: three bytes a character, and %u / %s between them.
    const std::string zh = formatText(
        "已收到 %u 条来自 %s 的报文，其中 %u 条校验失败；请检查天线、馈线和接收机的增益设置，"
        "并确认所选频率与当地的航空无线电频率规划一致，然后再次尝试接收。",
        17u, "ACARS", 3u);
    const std::string zhWant =
        "已收到 17 条来自 ACARS 的报文，其中 3 条校验失败；请检查天线、馈线和接收机的增益设置，"
        "并确认所选频率与当地的航空无线电频率规划一致，然后再次尝试接收。";
    CHECK(zh == zhWant);

    // An empty format is an empty string, and clears what was there.
    std::string empty = "was here";
    CHECK(formatUtf8(empty, "") == 0);
    CHECK(empty.empty());
    CHECK(formatText("").empty());
    // %% is one percent sign.
    CHECK(formatText("100%% %s", "ok") == "100% ok");
    std::string pct;
    CHECK(formatUtf8(pct, "%d%%", 50) == 3);
    CHECK(pct == "50%");
    // The destination may be an argument: the arguments are read before it
    // is written.
    std::string grow = "ab";
    formatUtf8(grow, "%s+%s", grow.c_str(), "x");
    CHECK(grow == "ab+x");
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

// THE NEXT ALPHABETS (0.99.28): the catalogues being translated now are
// written in Cyrillic, Greek, Vietnamese, Turkish, Romanian, the Baltic and
// Central European alphabets, and CJK. Each case below is a word a legend in
// that language could really carry.
void testUpperScripts() {
    std::printf("  upperLegend: Cyrillic, Greek, Vietnamese, Turkish and the rest\n");
    auto expect = [](const char* in, const char* want, const char* lang = "") {
        const std::string got = upperLegend(in, lang);
        if (got != want) {
            std::printf("      upperLegend(\"%s\", \"%s\") = \"%s\", want \"%s\"\n", in, lang,
                        got.c_str(), want);
        }
        CHECK(got == want);
    };
    // Cyrillic: Russian, Ukrainian, Bulgarian, and the Serbian / Macedonian
    // letters that sit in the U+0450 row rather than beside their capitals.
    expect("Настройки приёмника", "НАСТРОЙКИ ПРИЁМНИКА");
    expect("ґанок, їжак, єнот", "ҐАНОК, ЇЖАК, ЄНОТ");
    expect("ѝ ђ љ њ ћ џ ѓ ќ ѕ", "Ѝ Ђ Љ Њ Ћ Џ Ѓ Ќ Ѕ");
    // Greek: capitals, the final sigma, and the tonos that all-caps drops.
    expect("Ρυθμίσεις", "ΡΥΘΜΙΣΕΙΣ");
    expect("λόγος", "ΛΟΓΟΣ");
    expect("Έξοδος", "ΕΞΟΔΟΣ");  // an accented CAPITAL loses it too
    expect("Ευρώπη", "ΕΥΡΩΠΗ");
    expect("ΐ ΰ ϊ ϋ", "Ϊ Ϋ Ϊ Ϋ");  // the dialytika stays
    expect("προϊόν", "ΠΡΟΪΟΝ");
    // A dropped tonos must not create a diphthong the word does not have.
    expect("Μάιος", "ΜΑΪΟΣ");
    expect("άυλος", "ΑΫΛΟΣ");
    expect("ρολόι", "ΡΟΛΟΪ");
    expect("είναι", "ΕΙΝΑΙ");  // a real diphthong, unaccented: left alone
    expect("α\xCC\x81", "Α");  // decomposed: alpha + combining acute
    expect("e\xCC\x81", "E\xCC\x81");  // ...which is kept after a Latin letter
    // Vietnamese: the vowels with two marks, and ơ / ư from Latin Extended-B.
    expect("Tiếng Việt", "TIẾNG VIỆT");
    expect("Người dùng", "NGƯỜI DÙNG");
    expect("ạ ả ấ ầ ẩ ẫ ậ ắ ặ ẻ ẽ ế ệ ỉ ị ọ ỏ ố ồ ổ ỗ ộ ớ ờ ở ỡ ợ ụ ủ ứ ừ ử ữ ự ỳ ỵ ỷ ỹ đ",
           "Ạ Ả Ấ Ầ Ẩ Ẫ Ậ Ắ Ặ Ẻ Ẽ Ế Ệ Ỉ Ị Ọ Ỏ Ố Ồ Ổ Ỗ Ộ Ớ Ờ Ở Ỡ Ợ Ụ Ủ Ứ Ừ Ử Ữ Ự Ỳ Ỵ Ỷ Ỹ Đ");
    // Romanian (comma-below, Latin Extended-B), Czech, Slovak, Hungarian,
    // the Baltic languages, Catalan, Croatian digraphs, and ÿ.
    expect("setări: ș ț", "SETĂRI: Ș Ț");
    expect("řeč ůl ěž ľ ĺ ŕ ô", "ŘEČ ŮL ĚŽ Ľ Ĺ Ŕ Ô");
    expect("őrző űr", "ŐRZŐ ŰR");
    expect("ų į ė ū ģ ķ ļ ņ õ", "Ų Į Ė Ū Ģ Ķ Ļ Ņ Õ");
    expect("col·lecció ŀ", "COL·LECCIÓ Ŀ");
    expect("ǆ ǉ ǌ ǅ", "Ǆ Ǉ Ǌ Ǆ");
    expect("ÿ", "Ÿ");
    // Turkish and Azerbaijani keep the dot on i; nobody else does. Dotless ı
    // is I everywhere.
    expect("istasyon", "İSTASYON", "tr");
    expect("Bilgi", "BİLGİ", "tr-TR");
    expect("bilgi", "BİLGİ", "az-Latn");
    expect("istasyon", "ISTASYON");
    expect("istasyon", "ISTASYON", "tri");  // a different language that starts "tr"
    expect("ılık", "ILIK", "tr");
    expect("ılık", "ILIK");
    // Han, kana and Hangul have no case.
    expect("设置 繁體 ひらがな カタカナ 한국어", "设置 繁體 ひらがな カタカナ 한국어");
}

// THE COUNTRY LIST'S SEARCH. What the reader types is folded the same way as
// every name, so case and the usual accents never stop a match - in Cyrillic
// and Greek as much as in Latin. And the Latin keys are what they were, so the
// six shipped languages sort their country lists exactly as before.
void testFoldForSearch() {
    std::printf("  foldForSearch: case and accents off in Latin, Cyrillic, Greek, Vietnamese\n");
    using cascade::core::foldForSearch;
    auto finds = [](const char* typed, const char* name) {
        const bool hit = foldForSearch(name).find(foldForSearch(typed)) != std::string::npos;
        if (!hit) {
            std::printf("      \"%s\" does not find \"%s\" (\"%s\" in \"%s\")\n", typed, name,
                        foldForSearch(typed).c_str(), foldForSearch(name).c_str());
        }
        return hit;
    };
    // Latin: unchanged from the key the list always used.
    CHECK(foldForSearch("\xC3\x96sterreich") == "osterreich");
    CHECK(foldForSearch("C\xC3\xB4te d'Ivoire") == "cote d'ivoire");
    CHECK(foldForSearch("BRASIL") == "brasil");
    CHECK(finds("osterreich", "\xC3\x96sterreich"));
    // Cyrillic, either case typed.
    CHECK(finds("россия", "Россия"));
    CHECK(finds("РОССИЯ", "Россия"));
    CHECK(finds("україна", "Україна"));
    CHECK(finds("ЁЛКА", "ёлка"));
    CHECK(finds("бълг", "България"));
    // Greek: case and the tonos.
    CHECK(finds("ελλαδα", "Ελλάδα"));
    CHECK(finds("ΕΛΛΆΔΑ", "Ελλάδα"));
    CHECK(finds("κυπρος", "Κύπρος"));
    // Vietnamese, Romanian and Turkish letters beyond Latin Extended-A.
    CHECK(finds("viet nam", "Việt Nam"));
    CHECK(finds("VIỆT", "Việt Nam"));
    CHECK(finds("romania", "România"));
    CHECK(finds("tara", "Țara"));
    CHECK(finds("turkiye", "Türkiye"));
    // And a mismatch is still a mismatch.
    CHECK(foldForSearch("россия").find(foldForSearch("польша")) == std::string::npos);
    // Han and Hangul pass through, so a CJK name still finds itself.
    CHECK(finds("中国", "中国"));
    CHECK(finds("한민", "대한민국"));
}

void testUpper() {
    testUpperScripts();
    testFoldForSearch();
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
    testFormatWhole();
    testCopyVisible();
    testUpper();
    return testSummary("test_utf8_text");
}
