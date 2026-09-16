// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/band_plan_style.hpp"

#include "core/band_plan.hpp"
#include "gui/fonts.hpp"

namespace cascade::gui {

BandPlanSizeTier bandPlanSizeTierFromKey(const std::string& key) {
    if (key == "medium") { return BandPlanSizeTier::Medium; }
    if (key == "large") { return BandPlanSizeTier::Large; }
    return BandPlanSizeTier::Small;
}

BandPlanPaletteKind bandPlanPaletteKindFromKey(const std::string& key) {
    if (key == "vivid") { return BandPlanPaletteKind::Vivid; }
    if (key == "mono") { return BandPlanPaletteKind::Mono; }
    return BandPlanPaletteKind::Classic;
}

const char* bandPlanSizeKey(BandPlanSizeTier size) {
    switch (size) {
        case BandPlanSizeTier::Medium: return "medium";
        case BandPlanSizeTier::Large: return "large";
        case BandPlanSizeTier::Small: default: return "small";
    }
}

const char* bandPlanPaletteKey(BandPlanPaletteKind palette) {
    switch (palette) {
        case BandPlanPaletteKind::Vivid: return "vivid";
        case BandPlanPaletteKind::Mono: return "mono";
        case BandPlanPaletteKind::Classic: default: return "classic";
    }
}

BandRibbonGeometry bandRibbonGeometry(BandPlanSizeTier size, float scale) {
    // The overlay's original, fixed figures — see app_window.cpp's
    // kBandRibbonPx/kBandLabelMinPx history for why they are what they are.
    // The label size was never a named constant: AddText's no-font overload
    // draws at whatever the current ImGui font is, which fonts::load() binds
    // to ui() at kUiSize — spelled out explicitly here so "small" is a real
    // number this function owns rather than an ambient default.
    constexpr float kSmallRibbonPx = 6.0f;
    constexpr float kSmallLabelPx = cascade::gui::fonts::kUiSize;
    constexpr float kSmallLabelMinPx = 46.0f;

    float mul = 1.0f;
    switch (size) {
        case BandPlanSizeTier::Medium: mul = 1.6f; break;
        case BandPlanSizeTier::Large: mul = 2.5f; break;
        case BandPlanSizeTier::Small: default: mul = 1.0f; break;
    }

    BandRibbonGeometry g;
    g.ribbonPx = kSmallRibbonPx * mul * scale;
    g.labelPx = kSmallLabelPx * mul * scale;
    g.labelMinPx = kSmallLabelMinPx * mul * scale;
    return g;
}

namespace {

// service -> packed 0xRRGGBBAA, one table per non-classic palette. Kept as
// free functions (not a table indexed by service) because a service string
// the loader does not recognise falls through to the same "other" answer
// core::BandPlan::colorForService gives it — the overlay must never go blank
// on a service class a future band-plan file invents.

std::uint32_t vividColor(const std::string& service) {
    // A bolder alpha than classic's uniform 0x60: this palette exists for
    // "high-contrast", and the extra punch is still one uniform value across
    // every class, so no single service reads louder than its neighbours.
    constexpr std::uint32_t kAlpha = 0x80u;
    const auto rgba = [](std::uint32_t rgb) { return (rgb << 8) | kAlpha; };
    if (service == "broadcast") { return rgba(0xFF8C00u); }  // saturated orange
    if (service == "amateur") { return rgba(0x00B7FFu); }    // saturated azure
    if (service == "aviation") { return rgba(0x00E676u); }   // saturated green
    if (service == "marine") { return rgba(0x2979FFu); }     // saturated blue
    if (service == "mobile") { return rgba(0xFF3D00u); }     // saturated red-orange
    if (service == "satellite") { return rgba(0xE91E8Cu); }  // saturated magenta
    if (service == "iss") { return rgba(0xFFEA00u); }        // saturated yellow
    return rgba(0xBDBDBDu);  // unclassified: light grey, still plainly visible
}

std::uint32_t monoColor(const std::string& service) {
    // One accent hue - the instrument's own amber, matching theme::kAmber's
    // RGB (0xF0A840) - graded by alpha per class instead of by hue, so a
    // reader who finds the rainbow noisy still gets classes that are told
    // apart, by shade.
    constexpr std::uint32_t kAccentRgb = 0xF0A840u;
    const auto graded = [](std::uint32_t a) { return (kAccentRgb << 8) | a; };
    if (service == "broadcast") { return graded(0x90u); }
    if (service == "amateur") { return graded(0x80u); }
    if (service == "aviation") { return graded(0x70u); }
    if (service == "marine") { return graded(0x60u); }
    if (service == "mobile") { return graded(0x50u); }
    if (service == "satellite") { return graded(0x40u); }
    if (service == "iss") { return graded(0xA0u); }
    return graded(0x30u);  // unclassified: faintest of the ramp
}

}  // namespace

std::uint32_t bandPlanPaletteColor(BandPlanPaletteKind palette, const std::string& service) {
    switch (palette) {
        case BandPlanPaletteKind::Vivid: return vividColor(service);
        case BandPlanPaletteKind::Mono: return monoColor(service);
        case BandPlanPaletteKind::Classic:
        default:
            return cascade::core::BandPlan::colorForService(service);
    }
}

}  // namespace cascade::gui
