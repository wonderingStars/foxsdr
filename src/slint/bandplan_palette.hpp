// bandplan_palette.hpp - THE BAND PLAN'S THREE PALETTES (window-gaps-2, rows 258, 269, 270): SYSTEM > Display > Colour. The classic window's gui/band_plan_style.cpp (bandPlanPaletteColor) draws the ribbon of
// the band plan by SERVICE class, so switching Colour repaints every installed plan the same way: Classic keeps the plan file's own colour, Vivid is one bolder alpha over saturated hues, Mono is the
// instrument's accent graded by alpha per class. Those tables are quoted here (the classic file draws with ImGui and cannot be linked into this window's tests).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

namespace foxsdr::slintbands {

// A colour packed 0xRRGGBBAA, as the engine's band plan list carries it. `palette` is the saved key ("classic", "vivid", "mono"; anything else is Classic); `service` the band's class; `classicRgba` the plan
// file's own colour for it; `accentRgb` the skin's accent as 0xRRGGBB (Mono grades it).
std::uint32_t bandPlanColour(const std::string& palette, const std::string& service, std::uint32_t classicRgba, std::uint32_t accentRgb);

}  // namespace foxsdr::slintbands
