// font_blobs.hpp - the three embedded typefaces, reachable WITHOUT ImGui and
// without recompiling three megabytes of initialiser.
//
// WHY THIS EXISTS. gui/font_assets.hpp holds the faces as byte arrays and says
// in its own header to include it from exactly one translation unit, because it
// is about three megabytes of initialiser; gui/fonts.hpp is the other way to
// reach them and pulls in ImGui, which the web server has no business
// depending on. The remote interface needs the same bytes for a very different
// reason - to serve them to a browser so the page is lettered in the faces the
// bench is lettered in - and neither existing header can give it them.
//
// So: declarations only, no ImGui, no data. The definitions live in fonts.cpp,
// which already includes font_assets.hpp and is therefore still the only
// translation unit that compiles it.
//
// LICENCE, because it governs what may be done with what these return. Both
// families are under the SIL Open Font License and carry a Reserved Font Name,
// which permits redistribution - including serving them over HTTP - but NOT
// subsetting or any other modification while keeping their names. These
// accessors hand back the upstream files byte for byte, and every caller must
// pass them on unchanged. Licences travel in third_party/fonts/OFL-*.txt.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FONT_BLOBS_HPP
#define CASCADE_GUI_FONT_BLOBS_HPP

#include <cstddef>

namespace cascade::gui::fonts {

// Saira Condensed Medium - controls and prose. See fonts.hpp for the roles;
// this file only moves the bytes.
const unsigned char* uiTtf();
std::size_t uiTtfLen();

// Saira Condensed SemiBold - engraved captions and section plates.
const unsigned char* legendTtf();
std::size_t legendTtfLen();

// Nova Mono - digits on glass.
const unsigned char* readingTtf();
std::size_t readingTtfLen();

}  // namespace cascade::gui::fonts

#endif  // CASCADE_GUI_FONT_BLOBS_HPP
