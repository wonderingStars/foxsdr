// bandplan_palette.cpp - see bandplan_palette.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "bandplan_palette.hpp"

namespace foxsdr::slintbands {

namespace {

std::uint32_t vivid(const std::string& service) {
    // a bolder alpha than classic's uniform 0x60, the same for every class so no service reads louder than its neighbours
    constexpr std::uint32_t kAlpha = 0x80u;
    const auto rgba = [](std::uint32_t rgb) { return (rgb << 8) | kAlpha; };
    if (service == "broadcast") { return rgba(0xFF8C00u); }
    if (service == "amateur") { return rgba(0x00B7FFu); }
    if (service == "aviation") { return rgba(0x00E676u); }
    if (service == "marine") { return rgba(0x2979FFu); }
    if (service == "mobile") { return rgba(0xFF3D00u); }
    if (service == "satellite") { return rgba(0xE91E8Cu); }
    if (service == "iss") { return rgba(0xFFEA00u); }
    return rgba(0xBDBDBDu);   // unclassified: light grey, still plainly visible
}

std::uint32_t mono(const std::string& service, std::uint32_t accentRgb) {
    const auto graded = [accentRgb](std::uint32_t a) { return ((accentRgb & 0xFFFFFFu) << 8) | a; };
    if (service == "broadcast") { return graded(0x90u); }
    if (service == "amateur") { return graded(0x80u); }
    if (service == "aviation") { return graded(0x70u); }
    if (service == "marine") { return graded(0x60u); }
    if (service == "mobile") { return graded(0x50u); }
    if (service == "satellite") { return graded(0x40u); }
    if (service == "iss") { return graded(0xA0u); }
    return graded(0x30u);   // unclassified: faintest of the ramp
}

}  // namespace

std::uint32_t bandPlanColour(const std::string& palette, const std::string& service, std::uint32_t classicRgba, std::uint32_t accentRgb) {
    if (palette == "vivid") { return vivid(service); }
    if (palette == "mono") { return mono(service, accentRgb); }
    return classicRgba;
}

}  // namespace foxsdr::slintbands
