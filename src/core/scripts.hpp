// scripts.hpp - which WRITING SYSTEM a language needs, and which typeface on
// this machine can draw it. The one place that knowledge lives.
//
// WHY IT IS NEEDED. The first six catalogues were all written in the Latin
// alphabet, and every letter they used was in the faces compiled into the
// binary. The next set is not: Russian, Ukrainian and Bulgarian are Cyrillic,
// Greek is Greek, Vietnamese stacks two accents on one vowel, and Chinese,
// Japanese and Korean are tens of thousands of characters. Dear ImGui draws a
// character its font does not have as a box, and a whole interface of boxes
// is what a Japanese user would see if nothing here existed.
//
// TWO KINDS OF COVERAGE, and they are handled differently on purpose:
//
//   EVERYTHING BUT CJK is covered by typefaces COMPILED IN (gui/fonts.cpp: an
//   OFL Noto Sans subset with Latin Extended, Cyrillic and Greek behind the
//   bench's own faces), so a Russian or a Vietnamese catalogue draws on any
//   machine, Linux included. test_i18n_glyphs proves it for every catalogue.
//
//   CJK comes FROM THE OPERATING SYSTEM, never from the binary: one Chinese
//   face is 15-20 MB, a set of four would triple the download for everybody,
//   and every Windows ships the faces already (Microsoft YaHei, JhengHei, Yu
//   Gothic, Malgun Gothic). A Linux desktop may have none of them, and then a
//   CJK language is UNAVAILABLE - listed with the reason, never selected into
//   a screen of boxes. cjkFaceFor() says which face a language needs,
//   systemCandidates() where to look for one.
//
// NOTHING HERE READS A FONT. This file is data and pure functions, so the
// rules can be tested without a font, an atlas or a particular machine; the
// reading and the atlas are gui/font_probe.cpp and gui/fonts.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SCRIPTS_HPP
#define CASCADE_CORE_SCRIPTS_HPP

#include <string>
#include <string_view>
#include <vector>

namespace cascade::core::scripts {

// The system CJK typeface a language needs - one per written standard, not
// one per script, because the same Han character is DRAWN differently in
// Simplified Chinese, Traditional Chinese and Japanese (Unicode unifies them;
// the fonts do not), and a Japanese reader shown the Chinese forms sees a
// foreign text. Korean is its own because Hangul is in no Chinese or Japanese
// face.
enum class CjkFace {
    None,                // no CJK needed: the compiled-in faces cover it
    ChineseSimplified,   // zh-CN, zh-SG, zh-Hans, bare zh
    ChineseTraditional,  // zh-TW, zh-HK, zh-MO, zh-Hant
    Japanese,            // ja
    Korean,              // ko
};

// The ones there are, in a fixed order, for loops and tests.
inline constexpr CjkFace kCjkFaces[] = {CjkFace::ChineseSimplified, CjkFace::ChineseTraditional,
                                        CjkFace::Japanese, CjkFace::Korean};

// For logs: "Chinese (Simplified)".
const char* cjkFaceName(CjkFace f);

// Code points ONLY a CJK face draws: Hangul (jamo, compatibility jamo,
// syllables), kana, the CJK symbols and punctuation block, bopomofo, the
// unified and compatibility ideographs, CJK compatibility forms, and the
// fullwidth / halfwidth forms Chinese and Japanese use for their commas and
// colons. None of these is in the compiled-in faces, and everything outside
// them is expected to be.
bool isCjk(unsigned int cp);
bool isHangul(unsigned int cp);

// A LETTER of the Latin, Greek or Cyrillic alphabets (with its marks) - not a
// digit, a punctuation mark or a symbol. What the font chain requires of the
// typeface a whole language is lettered in: every letter from ONE face, so no
// word is assembled from two, while a dong sign or a guillemet may come from
// the fallback behind it without moving the whole interface to another face.
bool isLetter(unsigned int cp);
bool isKana(unsigned int cp);
bool isHan(unsigned int cp);

// Which face `languageCode` needs. By the code first - zh-TW needs the
// Traditional face even though its characters would ALSO be found in a
// Simplified one, drawn wrongly - and, for a code this does not know (a
// translator's file under a code nobody planned for), by what the catalogue
// is written in: Hangul means Korean, kana means Japanese, other Han means
// Simplified Chinese. `codePoints` may be empty; then only the code decides.
CjkFace cjkFaceFor(std::string_view languageCode, const std::vector<unsigned int>& codePoints);

// A handful of characters every usable face for `f` must have - the ones a
// catalogue in that standard is certain to use, including the ones that tell
// the standards apart (简 is Simplified, 繁 Traditional, ひ Japanese, 한
// Korean). A face that lacks any of them is not used for `f`. Empty for None.
std::vector<unsigned int> probeCodePoints(CjkFace f);

// Where a face for `f` may be. `windows` selects the list: on Windows a bare
// FILE NAME inside the system font directory (gui::fonts::systemFontPath
// makes the path), elsewhere an ABSOLUTE PATH in the places the common
// distributions install Noto Sans CJK, Source Han Sans, WenQuanYi, Droid Sans
// Fallback, IPA / Takao and Nanum - read directly, so no fontconfig is linked.
// In order of preference. `nameHint` picks the face inside a collection
// (.ttc): the one whose family or full name CONTAINS it, provided it has the
// probe characters; with no such face, the first one that has them.
struct SystemFace {
    std::string path;
    const char* nameHint;
};
std::vector<SystemFace> systemCandidates(CjkFace f, bool windows);

// Why a language needing `f` cannot be chosen on a machine with no usable
// face - an ENGLISH sentence that is also a translation key, so the caller
// draws tr() of it. Worded for the platform: on Linux it names the package
// that fixes it. Empty for None.
const char* missingFontReason(CjkFace f, bool windows);

}  // namespace cascade::core::scripts

#endif  // CASCADE_CORE_SCRIPTS_HPP
