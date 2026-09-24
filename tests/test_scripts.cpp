// Tests for core/scripts.hpp - which writing system a language needs and
// where a face for it may be - and for the rule i18n.hpp builds on it: a
// language this machine cannot draw is never the one in force.
//
// Pure: no font is read and no atlas made. The drawable predicate is a fake
// installed by the test, which is the point - "auto on a Japanese desktop with
// no Japanese font" can be asserted on any machine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/scripts.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "test_check.hpp"

namespace scripts = cascade::core::scripts;
using scripts::CjkFace;

namespace {

void testFaceForCode() {
    std::printf("  cjkFaceFor: the code decides, and the text when the code is unknown\n");
    const std::vector<unsigned int> none;
    CHECK(scripts::cjkFaceFor("zh-CN", none) == CjkFace::ChineseSimplified);
    CHECK(scripts::cjkFaceFor("zh", none) == CjkFace::ChineseSimplified);
    CHECK(scripts::cjkFaceFor("zh-SG", none) == CjkFace::ChineseSimplified);
    CHECK(scripts::cjkFaceFor("zh-Hans-HK", none) == CjkFace::ChineseSimplified);
    CHECK(scripts::cjkFaceFor("zh-TW", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("ZH-tw", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("zh-HK", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("zh-MO", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("zh-Hant", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("yue", none) == CjkFace::ChineseTraditional);
    CHECK(scripts::cjkFaceFor("ja", none) == CjkFace::Japanese);
    CHECK(scripts::cjkFaceFor("ja-JP", none) == CjkFace::Japanese);
    CHECK(scripts::cjkFaceFor("ko", none) == CjkFace::Korean);
    CHECK(scripts::cjkFaceFor("ko-KR", none) == CjkFace::Korean);
    // Every alphabet the compiled-in faces cover needs nothing from the system.
    for (const char* code : {"en", "pt-BR", "ru", "uk", "bg", "el", "vi", "tr", "zu", "sr-Cyrl"}) {
        CHECK(scripts::cjkFaceFor(code, none) == CjkFace::None);
    }
    // "zhx" is not "zh": the primary subtag is compared whole.
    CHECK(scripts::cjkFaceFor("zhx", none) == CjkFace::None);
    // An unknown code, by its text: Hangul, kana, then Han.
    CHECK(scripts::cjkFaceFor("xx", {0x41, 0xD55C}) == CjkFace::Korean);
    CHECK(scripts::cjkFaceFor("xx", {0x65E5, 0x3072}) == CjkFace::Japanese);
    CHECK(scripts::cjkFaceFor("xx", {0x4E2D}) == CjkFace::ChineseSimplified);
    CHECK(scripts::cjkFaceFor("xx", {0xFF0C}) == CjkFace::ChineseSimplified);  // a fullwidth comma
    CHECK(scripts::cjkFaceFor("xx", {0x41, 0x416, 0x3A9, 0x1EC7}) == CjkFace::None);
    // The code wins over the text: a Traditional catalogue is Traditional
    // even though every character in it is also in a Simplified face.
    CHECK(scripts::cjkFaceFor("zh-TW", {0x4E2D}) == CjkFace::ChineseTraditional);
}

void testClassification() {
    std::printf("  isCjk and friends: the block edges\n");
    struct Case {
        unsigned int cp;
        bool cjk;
    };
    const Case cases[] = {
        {0x41, false},    {0x2019, false},  {0x0416, false}, {0x03A9, false}, {0x1EC7, false},
        {0x20AB, false},  {0x2E7F, false},  {0x2E80, true},  {0x3000, true},  {0x3001, true},
        {0x3002, true},   {0x303F, true},   {0x3041, true},  {0x30FC, true},  {0x3105, true},
        {0x3131, true},   {0x4E00, true},   {0x9FFF, true},  {0xA000, false}, {0xAC00, true},
        {0xD7A3, true},   {0xF900, true},   {0xFE30, true},  {0xFF0C, true},  {0xFF1A, true},
        {0xFFEF, true},   {0xFFF0, false},  {0x20000, true}, {0x1100, true},  {0x10FF, false},
    };
    for (const Case& c : cases) {
        if (scripts::isCjk(c.cp) != c.cjk) { std::printf("      U+%04X\n", c.cp); }
        CHECK(scripts::isCjk(c.cp) == c.cjk);
    }
    // Letters decide which face a language is lettered in; signs do not.
    for (const unsigned int cp : {0x61u, 0x5Au, 0xE9u, 0x142u, 0x219u, 0x1A1u, 0x3C9u, 0x390u,
                                  0x416u, 0x45Du, 0x491u, 0x1EC7u}) {
        if (!scripts::isLetter(cp)) { std::printf("      U+%04X should be a letter\n", cp); }
        CHECK(scripts::isLetter(cp));
    }
    for (const unsigned int cp : {0x31u, 0x20u, 0x2Cu, 0xABu, 0xB7u, 0xD7u, 0xF7u, 0x37Eu, 0x387u,
                                  0x482u, 0x2116u, 0x20ABu, 0x2014u, 0x4E00u}) {
        if (scripts::isLetter(cp)) { std::printf("      U+%04X should not be a letter\n", cp); }
        CHECK(!scripts::isLetter(cp));
    }
    CHECK(scripts::isHangul(0xAC00) && scripts::isHangul(0x3131) && !scripts::isHangul(0x4E00));
    CHECK(scripts::isKana(0x3042) && scripts::isKana(0x30AB) && scripts::isKana(0xFF76) &&
          !scripts::isKana(0x4E00));
    CHECK(scripts::isHan(0x4E00) && scripts::isHan(0x3400) && !scripts::isHan(0x3042) &&
          !scripts::isHan(0xAC00));
}

void testProbesAndCandidates() {
    std::printf("  every CJK standard has a probe, a reason and somewhere to look\n");
    CHECK(scripts::probeCodePoints(CjkFace::None).empty());
    CHECK(std::strlen(scripts::missingFontReason(CjkFace::None, true)) == 0);
    CHECK(scripts::systemCandidates(CjkFace::None, false).empty());
    for (const CjkFace f : scripts::kCjkFaces) {
        const std::vector<unsigned int> probe = scripts::probeCodePoints(f);
        CHECK(probe.size() >= 6);
        for (const unsigned int cp : probe) { CHECK(scripts::isCjk(cp)); }
        // Each probe names the standard: the character that tells it apart.
        CHECK(scripts::cjkFaceFor("xx", probe) ==
              (f == CjkFace::ChineseTraditional ? CjkFace::ChineseSimplified : f));
        for (const bool windows : {true, false}) {
            const auto cands = scripts::systemCandidates(f, windows);
            CHECK(!cands.empty());
            for (const scripts::SystemFace& c : cands) {
                // Windows: a file name in the font directory; elsewhere a
                // full path (the $HOME entries are absolute in this process's
                // own terms, which on a Windows test run means a drive letter).
                const bool bare = c.path.find('/') == std::string::npos &&
                                  c.path.find('\\') == std::string::npos;
                const bool absolute = !c.path.empty() &&
                                      (c.path[0] == '/' || std::filesystem::path(c.path).is_absolute());
                if (windows ? !bare : !absolute) { std::printf("      %s\n", c.path.c_str()); }
                CHECK(windows ? bare : absolute);
                CHECK(c.nameHint != nullptr && c.nameHint[0] != '\0');
            }
            const char* why = scripts::missingFontReason(f, windows);
            CHECK(std::strlen(why) > 0);
            CHECK((std::strstr(why, "fonts-noto-cjk") != nullptr) == !windows);
        }
    }
    // The faces the owner named, first in their lists on Windows.
    CHECK(scripts::systemCandidates(CjkFace::ChineseSimplified, true).front().path == "msyh.ttc");
    CHECK(scripts::systemCandidates(CjkFace::ChineseTraditional, true).front().path == "msjh.ttc");
    CHECK(scripts::systemCandidates(CjkFace::Japanese, true).front().path == "YuGothM.ttc");
    CHECK(scripts::systemCandidates(CjkFace::Korean, true).front().path == "malgun.ttf");
    // And the Noto CJK collection first everywhere else, with the face named
    // for the standard - a .ttc holds all four.
    const auto jp = scripts::systemCandidates(CjkFace::Japanese, false);
    CHECK(jp.front().path == "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    CHECK(std::strcmp(jp.front().nameHint, "Noto Sans CJK JP") == 0);
}

// --- the drawable rule, through a fake predicate ---------------------------

bool gJapaneseDrawable = false;
bool fakeDrawable(const std::string& code) { return code != "ja" || gJapaneseDrawable; }

void addCatalogue(const char* code, const char* name, const char* english, const char* value) {
    const nlohmann::json doc = {{"code", code},
                                {"name", name},
                                {"englishName", english},
                                {"strings", {{"Band plan", value}, {"Save", value}}}};
    std::string error;
    CHECK(cascade::i18n::addCatalogue(doc.dump(), &error));
}

void testDrawableRule() {
    std::printf("  a language that cannot be drawn here is never the one in force\n");
    // 日本語 / バンドプラン
    addCatalogue("ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "Japanese",
                 "\xE3\x83\x90\xE3\x83\xB3\xE3\x83\x89\xE3\x83\x97\xE3\x83\xA9\xE3\x83\xB3");
    // With no predicate every catalogue draws (the state test_i18n runs in).
    cascade::i18n::setDrawablePredicate(nullptr);
    CHECK(cascade::i18n::drawable("ja"));
    CHECK(cascade::i18n::resolveFor("auto", "ja-JP") == "ja");
    CHECK(cascade::i18n::resolveFor("ja", "") == "ja");

    cascade::i18n::setDrawablePredicate(&fakeDrawable);
    gJapaneseDrawable = false;
    CHECK(!cascade::i18n::drawable("ja"));
    CHECK(cascade::i18n::drawable("en"));  // English always draws
    // "auto" on a Japanese desktop with no Japanese face: English, not boxes.
    CHECK(cascade::i18n::resolveFor("auto", "ja-JP") == "en");
    CHECK(cascade::i18n::resolveFor("", "ja") == "en");
    // A saved choice is not applied either...
    CHECK(cascade::i18n::resolveFor("ja", "") == "en");
    CHECK(cascade::i18n::resolveFor(" JA ", "") == "en");
    // ...nor offered as the country's language.
    CHECK(cascade::i18n::matchCatalogue("ja").empty());
    CHECK(cascade::i18n::matchCatalogue("ja-JP").empty());
    CHECK(cascade::i18n::setLanguage("ja") == "en");
    CHECK(cascade::i18n::current() == "en");
    // The same machine once the font is installed: everything as before.
    gJapaneseDrawable = true;
    CHECK(cascade::i18n::resolveFor("auto", "ja-JP") == "ja");
    CHECK(cascade::i18n::resolveFor("ja", "") == "ja");
    CHECK(cascade::i18n::matchCatalogue("ja-JP") == "ja");
    CHECK(cascade::i18n::setLanguage("ja") == "ja");
    CHECK(cascade::i18n::current() == "ja");
    CHECK(cascade::i18n::setLanguage("en") == "en");
    cascade::i18n::setDrawablePredicate(nullptr);
}

void testCodePoints() {
    std::printf("  codePoints: every character a catalogue draws, once, in order\n");
    CHECK(cascade::i18n::codePoints("en").empty());
    CHECK(cascade::i18n::codePoints("no-such").empty());
    addCatalogue("xq-test", "Ab\xC3\xA9", "Test", "ba\n\xE6\x97\xA5");  // name Abé; "ba\n日"
    const std::vector<unsigned int> cps = cascade::i18n::codePoints("xq-test");
    // A b é (name), a (value; b again), the newline dropped, 日.
    const std::vector<unsigned int> want = {'A', 'a', 'b', 0xE9, 0x65E5};
    if (cps != want) {
        std::printf("      got");
        for (unsigned int cp : cps) { std::printf(" U+%04X", cp); }
        std::printf("\n");
    }
    CHECK(cps == want);
    // The text decides the face for a code nobody planned for.
    CHECK(scripts::cjkFaceFor("xq-test", cps) == CjkFace::ChineseSimplified);
}

}  // namespace

int main() {
    testFaceForCode();
    testClassification();
    testProbesAndCandidates();
    testDrawableRule();
    testCodePoints();
    return testSummary("test_scripts");
}
