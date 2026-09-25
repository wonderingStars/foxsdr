// tuner_ink.hpp - the inks the counter's cells and switches are drawn in, in
// the theme in force: one place the painters in app_window.cpp and a test
// (tests/test_theme.cpp) both read, so what is tested is what is drawn.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_TUNER_INK_HPP
#define CASCADE_GUI_TUNER_INK_HPP

#include "gui/theme.hpp"
#include "gui/tune_control.hpp"

namespace cascade::gui {

// The ground a cell's figure is lettered on: the tube's glass at its rim
// (Nixie) or the flat cell (neon, plain).
inline ImU32 tunerCellGround(const TunerCellPaint& p) {
    return p.glassFurniture
               ? theme::toneHex(p.cellRgb, 255, theme::ink::DigitBgBot)
               : theme::toneHex(p.cellRgb, 255, theme::ink::DigitBg);
}

// The glow rings around a lit figure, at `alpha`.
inline ImU32 tunerGlowInk(const TunerCellPaint& p, int alpha) {
    return theme::toneHex(p.glowRgb, alpha, theme::ink::Digit);
}

// THE LIT FIGURE ITSELF, at `alpha` - and it must stay HOTTER THAN ITS GLOW.
//
// A glowing figure is a bright core inside a coloured halo: the Nixie's
// #FFB347 inside #FF8A1F, the neon's near-white #D6FEFF inside cyan #00D0FF.
// Anchoring the core to the digit role alone made both the digit colour in
// every theme but today's, so Night Watch's neon was one red smear - the core
// the same colour as its halo (repair round, 2026-09-25). So a glowing core is
// the digit taken toward the theme's SHEEN (white, or Night Watch's own red
// highlight, which keeps white off a night-adapted eye): as far as kCoreLift,
// and less where that would wash it out against its own cell (a dark figure
// on a light LCD), never less than kCoreLiftMin. Under today's palette toneMix
// returns the literal, so today's counter is bit for bit what it was. A plain
// figure has no glow and is simply the digit.
inline constexpr float kCoreLift = 0.6f;
inline constexpr float kCoreLiftMin = 0.1f;
inline ImU32 tunerFigureInk(const TunerCellPaint& p, int alpha) {
    const int r = static_cast<int>((p.digitRgb >> 16) & 0xFFu);
    const int g = static_cast<int>((p.digitRgb >> 8) & 0xFFu);
    const int b = static_cast<int>(p.digitRgb & 0xFFu);
    if (p.glowLayers == 0) { return theme::toneHex(p.digitRgb, alpha, theme::ink::Digit); }
    const ImU32 ground = tunerCellGround(p);
    float lift = kCoreLift;
    while (lift > kCoreLiftMin &&
           theme::contrastRatio(theme::toneMix(r, g, b, 255, theme::ink::Digit, theme::ink::Sheen,
                                               lift),
                                ground) < 4.5) {
        lift -= 0.05f;
    }
    return theme::toneMix(r, g, b, alpha, theme::ink::Digit, theme::ink::Sheen,
                          lift < kCoreLiftMin ? kCoreLiftMin : lift);
}

// The bezel the tubes and the switches stand in.
inline ImU32 tunerBezelGround() {
    return theme::toneHex(0x0b0b09, 255, theme::ink::DigitBgBot, theme::ink::DigitBgTop);
}

// THE UP / DN STENCILS, lettered on the bezel. They are plate ink, which reads
// on a dark bezel - and Field Radio's bezel is its LCD's own light green, where
// cream plate ink stood at 1.04:1 and the words all but vanished. legible()
// keeps the plate ink wherever it reads and darkens (or lightens) it only as
// far as the bezel it is actually on needs; today's ink is untouched.
inline ImU32 tunerStencilInk() {
    return theme::legible(theme::toneHex(0xd8d3b8, 255, theme::ink::PlateInk),
                          tunerBezelGround(), 4.5);
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TUNER_INK_HPP
