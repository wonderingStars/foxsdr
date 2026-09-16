// Band-plan overlay style: how big the ribbon draws and which colours it
// uses, as pure functions of the two settings introduced for GitHub issue #1
// ("Band plan I think needs to be a bit bigger on the top, would be nice to
// see a few options for size and maybe colour").
//
// THIS HEADER STAYS FREE OF ImGui TYPES ON PURPOSE, the same discipline
// spectrum_view.hpp documents for itself: every function here is a plain
// string/enum/float in, a plain struct/uint32 out, so
// tests/test_band_plan_style.cpp can pin the numbers with no graphics
// context at all.
//
// NOTE ON SCALE: this codebase has no DPI/UI-scale layer anywhere else (no
// `gui::px()` or equivalent exists in src/ as of this writing — checked by
// grep across the whole tree) — every panel in the application is laid out
// in literal pixel constants (see gui/fonts.hpp's kUiSize etc.). The `scale`
// parameter below is therefore this function's own multiplier, not a hook
// into some wider system: app_window.cpp always calls it with 1.0f today.
// It exists, and is tested at 1.0 and 2.0, so the geometry math is proven
// correct for a caller that DOES have a scale to apply later, without this
// change inventing a scaling system the rest of the GUI does not have.
#pragma once

#include <cstdint>
#include <string>

namespace cascade::gui {

// The three ribbon sizes. Small reproduces the overlay's original, fixed
// figures (kBandRibbonPx == 6.0f and kBandLabelMinPx == 46.0f in
// app_window.cpp, and the label's implicit font size, fonts::kUiSize) —
// byte-identical — so an existing install's screenshot does not move a
// single pixel until the user opens the picker.
enum class BandPlanSizeTier { Small, Medium, Large };

// The three segment-colour palettes. Classic reproduces
// core::BandPlan::colorForService's Okabe-Ito colour-blind-safe set exactly.
enum class BandPlanPaletteKind { Classic, Vivid, Mono };

// Parses AppConfig::bandPlanSize / AppConfig::bandPlanPalette's string keys.
// Anything unrecognised — absent, hand-edited garbage, a future value this
// build does not know — falls back to Small / Classic, the same fallback
// ConfigStore::load applies to the fields themselves, so the two can never
// disagree about what an unknown value means.
BandPlanSizeTier bandPlanSizeTierFromKey(const std::string& key);
BandPlanPaletteKind bandPlanPaletteKindFromKey(const std::string& key);

// The inverse of the two functions above: the canonical AppConfig string for
// a tier/palette, so app_window.cpp has one place that owns the spelling in
// both directions.
const char* bandPlanSizeKey(BandPlanSizeTier size);
const char* bandPlanPaletteKey(BandPlanPaletteKind palette);

// The ribbon's geometry: how tall the coloured strip along the top of the
// spectrum draws, what size its band-name labels are lettered at, and the
// narrowest band (in the same px unit) that still gets a label drawn at all.
struct BandRibbonGeometry {
    float ribbonPx = 0.0f;
    float labelPx = 0.0f;
    float labelMinPx = 0.0f;
};

// Pure geometry for one size tier, scaled by `scale` (see the file header
// for what that parameter is and is not). Medium and large are ~1.6x and
// ~2.5x small's figures in every one of the three numbers, so a bigger
// ribbon also gets a legible label at a legible minimum width rather than
// growing taller with the same small print inside it.
BandRibbonGeometry bandRibbonGeometry(BandPlanSizeTier size, float scale = 1.0f);

// The colour a band of the given (coarse) service class draws in, under the
// given palette, packed 0xRRGGBBAA exactly like core::BandEntry::colorRgba —
// the overlay recolours per palette rather than per plan, so switching the
// setting repaints every installed plan identically.
//
//   classic  core::BandPlan::colorForService's set, unchanged.
//   vivid    saturated, higher-contrast hues at a bolder uniform alpha.
//   mono     one accent hue (the instrument amber) at a graded alpha per
//            class, for a reader who finds the classic rainbow noisy —
//            classes stay distinguishable by shade, not by hue.
std::uint32_t bandPlanPaletteColor(BandPlanPaletteKind palette, const std::string& service);

}  // namespace cascade::gui
