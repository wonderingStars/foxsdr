// font_blobs.hpp - the three embedded typefaces, reachable WITHOUT ImGui and
// without recompiling three megabytes of initialiser.
//
// WHY THIS EXISTS. core/font_assets.hpp holds the faces as byte arrays and says
// in its own header to include it from exactly one translation unit, because it
// is about three megabytes of initialiser; gui/fonts.hpp is the window's way to
// reach them and pulls in ImGui, which the web server has no business
// depending on. The remote interface needs the same bytes for a very different
// reason - to serve them to a browser so the page is lettered in the faces the
// bench is lettered in - and neither of those headers can give it them.
//
// So: declarations only, no ImGui, no data. The definitions live in
// core/font_blobs.cpp, which is the only translation unit that includes
// font_assets.hpp. ENGINE SIDE since the engine was split from the window
// (engine extraction, step 1): these declarations, their definitions and the
// bytes moved here from gui/ unchanged, because the web server is engine side
// and may not include a window header; gui/fonts.hpp brings the names back
// into cascade::gui::fonts, and gui/fonts.cpp builds its chains from them.
//
// LICENCE, because it governs what may be done with what these return. Both
// families are under the SIL Open Font License and carry a Reserved Font Name,
// which permits redistribution - including serving them over HTTP - but NOT
// subsetting or any other modification while keeping their names. These
// accessors hand back the upstream files byte for byte, and every caller must
// pass them on unchanged. Licences travel in third_party/fonts/OFL-*.txt.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FONT_BLOBS_HPP
#define CASCADE_CORE_FONT_BLOBS_HPP

#include <cstddef>

namespace cascade::core::fonts {

// Saira Condensed Medium - controls and prose. See gui/fonts.hpp for the
// roles; this file only moves the bytes.
const unsigned char* uiTtf();
std::size_t uiTtfLen();

// Saira Condensed SemiBold - engraved captions and section plates.
const unsigned char* legendTtf();
std::size_t legendTtfLen();

// Nova Mono - digits on glass.
const unsigned char* readingTtf();
std::size_t readingTtfLen();

// Noto Sans Condensed Medium and SemiBold - the fallback behind the faces
// above for Cyrillic, Greek and Vietnamese (gui/fonts.hpp). These two ARE a
// subset (tools/subset-noto.py), which their licence permits: Noto reserves
// no name.
const unsigned char* fallbackUiTtf();
std::size_t fallbackUiTtfLen();
const unsigned char* fallbackLegendTtf();
std::size_t fallbackLegendTtfLen();

}  // namespace cascade::core::fonts

#endif  // CASCADE_CORE_FONT_BLOBS_HPP
