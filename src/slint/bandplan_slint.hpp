// bandplan_slint.hpp - a band plan colour (0xRRGGBBAA, bandplan_palette.hpp) as the ribbon segment's Slint colour; 0 stays transparent, which the ribbon draws as the theme's own band colour.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

#include <slint.h>

#include "bandplan_palette.hpp"

namespace foxsdr::slintbands {

inline slint::Color ribbonColour(std::uint32_t rgba) {
    if (rgba == 0u) { return slint::Color::from_argb_uint8(0, 0, 0, 0); }
    return slint::Color::from_argb_uint8(static_cast<std::uint8_t>(rgba & 0xFFu), static_cast<std::uint8_t>((rgba >> 24) & 0xFFu), static_cast<std::uint8_t>((rgba >> 16) & 0xFFu), static_cast<std::uint8_t>((rgba >> 8) & 0xFFu));
}

}  // namespace foxsdr::slintbands
