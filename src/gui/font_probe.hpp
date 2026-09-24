// font_probe.hpp - asking a TrueType / OpenType file what it can draw,
// without an ImGui atlas.
//
// WHY NOT ImFont::IsGlyphInFont. That asks a font already IN the atlas, and
// the questions this answers come before anything is added: "which face in
// this .ttc is the Traditional Chinese one", "does the system's Yu Gothic
// really have kana", "which of Georgia and Saira has every letter of the
// Vietnamese catalogue". Adding a 20 MB collection to the atlas to find out
// that it is the wrong one would cost the very memory the lazy loading in
// fonts.cpp exists to save.
//
// The reader is stb_truetype - the same one ImGui rasterises with, compiled
// privately into font_probe.cpp (static linkage, so it cannot collide with
// imgui_draw.cpp's copy) - so a face this calls usable is a face the atlas
// can use, CFF-outlined OpenType collections included.
//
// A Face is a view: the bytes belong to the caller and must outlive it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FONT_PROBE_HPP
#define CASCADE_GUI_FONT_PROBE_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::gui::fontprobe {

struct Face {
    const unsigned char* data = nullptr;
    std::size_t len = 0;
    int index = 0;  // the face inside a collection; 0 for a single font
};

// The number of faces in the file: 1 for a .ttf / .otf, n for a .ttc, 0 for
// anything that is not a font stb_truetype can read.
int faceCount(const unsigned char* data, std::size_t len);

// The face's family, full and typographic family names (name IDs 1, 4, 16),
// English records first; the ones present, as UTF-8.
std::vector<std::string> names(const Face& f);

// Whether the face maps `cp` to a glyph.
bool has(const Face& f, unsigned int cp);

// How many of `cps` the face does NOT map; the first of them in *first when
// given and any are missing. A face that cannot be read misses all of them.
std::size_t missing(const Face& f, const std::vector<unsigned int>& cps,
                    unsigned int* first = nullptr);

// The face's ImGui pixel size in EMS: ImGui sizes a face so that ascent minus
// descent is the requested pixel height, so a face whose ascent and descent
// span 1.32 em draws its em smaller than one whose span 1.14 at the same
// size. The ratio of two faces' values is what makes a fallback's letters the
// same size as the primary's. 0 for a face that cannot be read.
float emSpan(const Face& f);

// The face in a file (a collection or a single font) to use for a script:
// the first whose names CONTAIN `hint` and which has every one of `probe`;
// failing that, the first that has every one of `probe`; -1 when none does.
int pickFace(const unsigned char* data, std::size_t len, std::string_view hint,
             const std::vector<unsigned int>& probe);

}  // namespace cascade::gui::fontprobe

#endif  // CASCADE_GUI_FONT_PROBE_HPP
