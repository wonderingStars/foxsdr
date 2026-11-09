// THE GLYPH COVERAGE OF THE SLINT WINDOW, OFFLINE (docs 6.62): no window, no renderer, only the font files and the catalogues compiled into the engine. For every one of the 32 translated
// languages (resources/lang, as the engine carries them):
//
//   1. every code point the catalogue draws that no CJK face is needed for - Latin with its accents, Greek, Cyrillic, the punctuation - is mapped by Inter, in all three weights the
//      window uses (Regular, Medium, SemiBold: third_party/fonts, the files the Slint compiler embeds), so no word is drawn from two typefaces and no box appears on a machine with no font
//      of its own. A code point Inter lacks is named, with its language;
//   2. for a language written in Han, kana or Hangul, the system face the window names (cjk_fonts.cpp over the classic's scripts.hpp) maps EVERY CJK code point of the catalogue - the
//      window names one family for the whole interface, so the first face has to carry the lot (the classic may merge up to three). A machine with no usable face is reported as SKIPPED,
//      never as passed; test_slint_cjk renders the same characters through the real Slint text engine.
//
// The checker is itself held: an unassigned code point is reported missing by the Inter check, and the Latin catalogues are shown not to need a CJK face at all.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "cjk_fonts.hpp"
#include "core/i18n.hpp"
#include "core/scripts.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace scripts = cascade::core::scripts;
namespace sf = foxsdr::slintfonts;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Embedded {
    const char* file;
    Bytes data;
};

// The code points `cps` that none of `faces` maps (index 0 of each file).
std::vector<unsigned int> unmappedBy(const std::vector<Embedded>& faces, const std::vector<unsigned int>& cps, std::size_t* whichFace = nullptr) {
    std::vector<unsigned int> out;
    for (const unsigned int cp : cps) {
        for (std::size_t i = 0; i < faces.size(); ++i) {
            if (!sf::bytesHave(faces[i].data.data(), faces[i].data.size(), 0, cp)) {
                out.push_back(cp);
                if (whichFace != nullptr) { *whichFace = i; }
                break;
            }
        }
    }
    return out;
}

std::string hexList(const std::vector<unsigned int>& v) {
    std::string s;
    char buf[16];
    for (std::size_t i = 0; i < v.size() && i < 12; ++i) {
        std::snprintf(buf, sizeof(buf), "%sU+%04X", i == 0 ? "" : " ", v[i]);
        s += buf;
    }
    if (v.size() > 12) { s += " ..."; }
    return s;
}

}  // namespace

int main() {
    std::printf("test_slint_host_glyphs\n");
    const std::string dir = FOXSDR_GLYPH_FONT_DIR;
    std::vector<Embedded> inter;
    for (const char* f : {"Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf"}) { inter.push_back(Embedded{f, readBytes(dir + "/" + f)}); }
    for (const Embedded& e : inter) {
        std::printf("  %s: %zu bytes\n", e.file, e.data.size());
        CHECK(e.data.size() > 100000);
    }

    // The checker is held first: Latin letters are mapped, an unassigned code point and a Han character are not.
    CHECK(unmappedBy(inter, {'A', 'z', 0xE9, 0x142, 0x44F, 0x3B1}).empty());   // A z e-acute l-stroke ya alpha
    CHECK(unmappedBy(inter, {0x0378, 0x10FFFD}).size() == 2);                 // unassigned code points are not mapped
    CHECK(unmappedBy(inter, {0x503C}).size() == 1);                           // a Han character is the CJK face's, not Inter's

    int translated = 0, cjkChecked = 0, cjkSkipped = 0;
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (l.code == "en") { continue; }
        ++translated;
        const std::vector<unsigned int> all = cascade::i18n::codePoints(l.code);
        std::vector<unsigned int> latin, cjk;
        for (const unsigned int cp : all) { (scripts::isCjk(cp) ? cjk : latin).push_back(cp); }
        CHECK(!all.empty());

        const std::vector<unsigned int> missingInter = unmappedBy(inter, latin);
        if (!missingInter.empty()) { std::printf("  %s: %zu code points Inter lacks: %s\n", l.code.c_str(), missingInter.size(), hexList(missingInter).c_str()); }
        CHECK(missingInter.empty());

        const scripts::CjkFace standard = scripts::cjkFaceFor(l.code, cjk);
        if (standard == scripts::CjkFace::None) {
            CHECK(cjk.empty());   // a Latin, Greek or Cyrillic catalogue needs no CJK face (one stray "，" would otherwise make the language depend on a system font)
            continue;
        }
        const std::vector<sf::Face> faces = sf::chooseFacesFor(l.code, cjk);
        if (faces.empty()) {
            std::printf("  %s: SKIPPED - this machine has no usable system face for %s\n", l.code.c_str(), scripts::cjkFaceName(standard));
            ++g_checksSkipped;
            ++cjkSkipped;
            continue;
        }
        ++cjkChecked;
        const Bytes first = readBytes(faces.front().path);
        CHECK(!first.empty());
        std::vector<unsigned int> missing;
        for (const unsigned int cp : cjk) {
            if (!sf::bytesHave(first.data(), first.size(), faces.front().index, cp)) { missing.push_back(cp); }
        }
        std::printf("  %s: %zu CJK code points, face %s \"%s\", %zu not in it%s%s\n", l.code.c_str(), cjk.size(), faces.front().label.c_str(), sf::familyOf(faces.front()).c_str(),
                    missing.size(), missing.empty() ? "" : ": ", hexList(missing).c_str());
        CHECK(missing.empty());
        CHECK(!sf::familyOf(faces.front()).empty());
    }
    // WHETHER A LANGUAGE CAN BE DRAWN (the predicate main.cpp installs): a Latin one always; a CJK one exactly when a usable face exists; a machine with no font folder at all has none, and
    // the language list then says why.
    CHECK(sf::languageDrawable("de"));
    CHECK(sf::languageDrawable("ru"));
    CHECK(sf::languageUnavailableReason("de").empty());
    for (const char* code : {"ja", "ko", "zh-CN", "zh-TW"}) {
        CHECK(sf::languageDrawable(code) == !sf::chooseFaces(code).empty());
        CHECK(sf::languageDrawable(code) == sf::languageUnavailableReason(code).empty());
        const std::vector<sf::Face> none = sf::chooseFaces(code, true, "Z:\\no-such-font-folder");
        CHECK(none.empty());   // no folder, no face: what a machine with no CJK font looks like to the chooser
    }
    CHECK(std::string(scripts::missingFontReason(scripts::CjkFace::Japanese, true)).size() > 20);
    std::printf("  %d translated languages, %d CJK checked, %d skipped\n", translated, cjkChecked, cjkSkipped);
    CHECK(translated >= 32);
    return testSummary("test_slint_host_glyphs");
}
