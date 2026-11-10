// THE CJK FACES OF THE SLINT WINDOW (docs/engine-stage4.md 6.62).
//
// THE PROBLEM. The two typefaces the window embeds (Inter, JetBrains Mono) have Latin, Greek and Cyrillic and no Han, kana or Hangul, so a Japanese, Korean or
// Chinese catalogue drew whatever the platform's own fallback chose for each missing character - the wrong regional forms of the Han characters, and a box
// for the characters that fallback face lacks (zh-TW on a machine without Microsoft JhengHei drew "峰值" as "峰" and a bar). The release notes of 0.99.108 list it.
//
// THE RULE IS THE CLASSIC WINDOW'S (core/scripts.hpp, gui/fonts.cpp cjkFacesFor): a CJK face comes from the OPERATING SYSTEM and is never shipped (one face is 15-20 MB and
// the licence is the system's), found by scripts::systemCandidates in its order of preference, usable when a face in the file has every probe character of the standard
// (gui/font_probe.hpp), the first one always taken and a later one (three at most) only for what the first lacks of the catalogue in force. This unit makes that choice
// WITHOUT ImGui - it reads the files and answers with paths and collection indexes - and main.cpp hands the answers to the window (register_font_from_path).
//
// The choice is pure of the window, so the offline test (test_slint_host_glyphs) asks it for every catalogue and proves every code point the catalogue uses is
// drawn by Inter, JetBrains Mono or a face in the answer - or reports the catalogue as having no usable face on the machine, never as passed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace foxsdr::slintfonts {

// One usable system face: the file, the face inside it (a .ttc holds several), and a label for the log ("msyh.ttc#1").
struct Face {
    std::string path;
    int index = 0;
    std::string label;
    std::vector<std::string> names;   // the face's family, full and typographic names, English first (fontprobe::names) - what a font-family property can say
};

// The CJK faces a language needs, for the code points the catalogue uses that only a CJK face draws (scripts::isCjk). Empty for a language that needs none and for a
// machine with no usable face. `windows` selects the candidate list as scripts::systemCandidates does; `fontDir` is the Windows font directory (empty: %WINDIR%\Fonts).
std::vector<Face> chooseFaces(std::string_view languageCode, bool windows = true, const std::string& fontDir = std::string());

// The same for a caller that already has the code points (the test, with a probe catalogue).
std::vector<Face> chooseFacesFor(std::string_view languageCode, const std::vector<unsigned int>& codePoints, bool windows = true, const std::string& fontDir = std::string());

// WHETHER A LANGUAGE CAN BE DRAWN HERE, as the classic's font layer answers cascade::i18n::drawable: a language that needs no CJK face always can, a CJK one only when this machine has a
// usable face. Cached per code (the first answer reads the candidate font files, 12 to 21 MB each, once). main.cpp installs it with cascade::i18n::setDrawablePredicate, so a saved
// Japanese on a machine with no Japanese face is KEPT but not applied, and the language list greys it with languageUnavailableReason beside the name.
bool languageDrawable(const std::string& languageCode);

// The English sentence (a translation key, scripts::missingFontReason) why `languageCode` cannot be drawn here; empty when it can.
std::string languageUnavailableReason(const std::string& languageCode);

// The family name a font-family property says for this face (docs 6.62): the typographic family (name ID 16) when the face has one - "Yu Gothic" for "Yu Gothic Medium" -,
// else its family (ID 1); the last of the names fontprobe answers, which are family, full name, typographic family in that order. Empty for a face with no names.
std::string familyOf(const Face& face);

// Whether the face (read from disk) maps the code point. The test's question; false for a file that cannot be read.
bool faceHas(const Face& face, unsigned int codePoint);

// Whether a file's bytes (an embedded font, read by the test) map the code point, for face `index`.
bool bytesHave(const unsigned char* data, std::size_t len, int index, unsigned int codePoint);

}  // namespace foxsdr::slintfonts
