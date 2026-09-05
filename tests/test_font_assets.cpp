/*
 * The three typefaces the interface is lettered in are VENDORED under
 * third_party/fonts and COMPILED IN from src/gui/font_assets.hpp, which
 * tools/embed-fonts.py generates. This test proves the compiled-in bytes are
 * still the vendored files, byte for byte.
 *
 * It is here for two reasons, and the second is the one that makes it more
 * than a tidiness check.
 *
 * FIRST, LAYOUT. Every hard-coded width in the interface - the transport
 * keys, the frequency field, the scanner's boxes, the left rail - was measured
 * against these faces at these sizes. A font that changed underneath them
 * does not fail; it produces legends that overhang their keys and a rail that
 * clips its own captions, on somebody else's machine, with nothing in the
 * build to say so.
 *
 * SECOND, LICENSING, and this one is not recoverable after the fact. Both
 * families carry a Reserved Font Name under the SIL Open Font License -
 * "Saira" and "NovaMono" - and clause 3 forbids a MODIFIED version from using
 * the reserved name. Subsetting the fonts down to the ASCII this application
 * actually draws is an obvious optimisation, saves about 400 KB, and would
 * make every shipped binary a licence violation unless the fonts were renamed
 * in their own name tables first. A test that pins the bytes is the only thing
 * standing between that idea and a release.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <fstream>
#include <string>

#include "gui/font_assets.hpp"
#include "test_check.hpp"

namespace {

// Read a file EXACTLY. No CRLF normalisation: these are binary, and a TTF is
// full of 0x0d 0x0a pairs that mean nothing about line endings. Normalising
// here would corrupt the comparison and, worse, would make it pass against a
// corrupted embed.
bool readExact(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

void reportFirstDifference(const char* what, const std::string& disk,
                           const std::string& baked) {
    const std::size_t n = disk.size() < baked.size() ? disk.size() : baked.size();
    std::size_t i = 0;
    while (i < n && disk[i] == baked[i]) { ++i; }
    std::printf("      %s: first difference at byte %zu; "
                "on disk %zu bytes, embedded %zu bytes\n",
                what, i, disk.size(), baked.size());
    std::printf("      re-run: py -3.14 tools/embed-fonts.py\n");
}

void checkOne(const std::string& root, const char* name, const unsigned char* bytes,
              unsigned int len) {
    const std::string path = root + "/third_party/fonts/" + name;
    std::string disk;
    if (!readExact(path, disk)) {
        std::printf("      cannot read %s\n", path.c_str());
        CHECK(false);
        return;
    }
    const std::string baked(reinterpret_cast<const char*>(bytes), len);
    // Compared WHOLE rather than by size and then by content: a size check
    // that passes proves nothing, and one that fails hides where.
    const bool same = (disk == baked);
    if (!same) { reportFirstDifference(name, disk, baked); }
    CHECK(same);
}

void testEmbeddedMatchesVendored(const std::string& root) {
    std::printf("  the compiled-in typefaces are the ones in third_party/fonts\n");
    checkOne(root, "SairaCondensed-Medium.ttf", cascade::gui::fontdata::kFontUiTtf,
             cascade::gui::fontdata::kFontUiTtfLen);
    checkOne(root, "SairaCondensed-SemiBold.ttf", cascade::gui::fontdata::kFontLegendTtf,
             cascade::gui::fontdata::kFontLegendTtfLen);
    checkOne(root, "NovaMono.ttf", cascade::gui::fontdata::kFontReadingTtf,
             cascade::gui::fontdata::kFontReadingTtfLen);
}

// A file that is not a TrueType font at all would still satisfy the comparison
// above if both copies were equally wrong - a truncated download vendored and
// then faithfully embedded, say. So the shape is checked too: the sfnt version
// tag every TrueType outline font starts with, and a size that could plausibly
// hold one.
void testTheyAreActuallyFonts() {
    std::printf("  each embedded blob is a TrueType font, not merely identical to a file\n");
    struct Face {
        const char* name;
        const unsigned char* bytes;
        unsigned int len;
    };
    const Face faces[] = {
        {"ui", cascade::gui::fontdata::kFontUiTtf, cascade::gui::fontdata::kFontUiTtfLen},
        {"legend", cascade::gui::fontdata::kFontLegendTtf,
         cascade::gui::fontdata::kFontLegendTtfLen},
        {"reading", cascade::gui::fontdata::kFontReadingTtf,
         cascade::gui::fontdata::kFontReadingTtfLen},
    };
    for (const Face& f : faces) {
        // 0x00010000 is the sfnt version of a TrueType outline font; "OTTO"
        // would be CFF outlines, which stb_truetype cannot rasterise, so it is
        // deliberately not accepted here.
        const bool tag = f.len > 4u && f.bytes[0] == 0x00 && f.bytes[1] == 0x01 &&
                         f.bytes[2] == 0x00 && f.bytes[3] == 0x00;
        if (!tag) {
            std::printf("      %s: not a TrueType sfnt (first bytes %02x %02x %02x %02x)\n",
                        f.name, f.len > 0u ? f.bytes[0] : 0u, f.len > 1u ? f.bytes[1] : 0u,
                        f.len > 2u ? f.bytes[2] : 0u, f.len > 3u ? f.bytes[3] : 0u);
        }
        CHECK(tag);
        CHECK(f.len > 20000u);
    }
}

// The reserved names must still be IN the fonts. If someone ever does subset
// them, the name table is where the rename would have to happen, so a missing
// name is the signal that the bytes are no longer the upstream article even if
// they still parse.
void testReservedNamesArePresent() {
    std::printf("  the reserved font names are intact, so these are still the upstream faces\n");
    const std::string ui(reinterpret_cast<const char*>(cascade::gui::fontdata::kFontUiTtf),
                         cascade::gui::fontdata::kFontUiTtfLen);
    const std::string reading(
        reinterpret_cast<const char*>(cascade::gui::fontdata::kFontReadingTtf),
        cascade::gui::fontdata::kFontReadingTtfLen);
    // Name records are UTF-16BE in the Windows platform encoding, so the ASCII
    // appears with a null between every character.
    const std::string kSaira("S\0a\0i\0r\0a\0", 10);
    const std::string kNova("N\0o\0v\0a\0", 8);
    CHECK(ui.find(kSaira) != std::string::npos);
    CHECK(reading.find(kNova) != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_font_assets <repository root>\n");
        return 2;
    }
    const std::string root = argv[1];
    testEmbeddedMatchesVendored(root);
    testTheyAreActuallyFonts();
    testReservedNamesArePresent();
    return testSummary("test_font_assets");
}
