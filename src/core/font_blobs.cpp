// font_blobs.cpp - the definitions behind core/font_blobs.hpp, and the one
// translation unit that compiles core/font_assets.hpp.
//
// These were at the bottom of gui/fonts.cpp until the engine was split from
// the window (engine extraction, step 1); moved here unchanged, because the web
// server - engine side - serves the same bytes and may not link the window.
//
// The first three return the upstream files unchanged, which is what the SIL
// Open Font License requires of a Reserved Font Name: redistribution yes,
// modification while keeping the name no. The two fallback faces are the
// subset tools/subset-noto.py cuts; Noto reserves no name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/font_blobs.hpp"

#include "core/font_assets.hpp"

namespace cascade::core::fonts {

const unsigned char* uiTtf() { return fontdata::kFontUiTtf; }
std::size_t uiTtfLen() { return fontdata::kFontUiTtfLen; }

const unsigned char* legendTtf() { return fontdata::kFontLegendTtf; }
std::size_t legendTtfLen() { return fontdata::kFontLegendTtfLen; }

const unsigned char* readingTtf() { return fontdata::kFontReadingTtf; }
std::size_t readingTtfLen() { return fontdata::kFontReadingTtfLen; }

const unsigned char* fallbackUiTtf() { return fontdata::kFontFallbackUiTtf; }
std::size_t fallbackUiTtfLen() { return fontdata::kFontFallbackUiTtfLen; }

const unsigned char* fallbackLegendTtf() { return fontdata::kFontFallbackLegendTtf; }
std::size_t fallbackLegendTtfLen() { return fontdata::kFontFallbackLegendTtfLen; }

}  // namespace cascade::core::fonts
