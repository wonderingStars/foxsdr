// ui_style_compose.hpp - the one function a theme change and an interface-
// scale change both funnel through, so applying either one, in either order,
// converges on the same live ImGuiStyle.
//
// THE BUG THIS CLOSES (an Opus review, round 4, "B1"). theme::applyTheme()
// (theme.cpp) writes UNSCALED sizes (WindowPadding, FramePadding, ...) and
// the NEW theme's colours straight into the live ImGuiStyle - it has no idea
// the interface can be scaled. gui/app_window.cpp's applyPendingUiScale used
// to keep its own "unscaled baseline" snapshot as a function-local static,
// captured ONCE, ever, on that function's very first call. So: pick Daylight
// Lab, then 150% - the scale change rebuilt the style from the STALE
// baseline (whatever theme was live the first time the function ever ran,
// typically "Today"), and Daylight Lab's colours vanished. And picking a
// theme while already at 150% reset the padding to 100%, because applyTheme
// overwrote the live style with its own unscaled numbers and nothing
// afterwards ever rescaled them.
//
// THE FIX. Both applyPendingTheme and applyPendingUiScale (app_window.cpp)
// now re-baseline (capture the CURRENT, just-written style as the new
// "unscaled" snapshot) and then call composeStyle() below to rebuild the
// live style from that fresh baseline at the CURRENT interface factor -
// every time either one changes. Composing is a pure function of two
// inputs (the baseline, the factor), so it does not matter which one
// changed most recently: the result is always "this theme's colours,
// these unscaled sizes, times the CURRENT S" - see tests/test_ui_scale.cpp's
// testThemeAndScaleComposeRegardlessOfOrder for the order-independence
// property this buys, checked against the real cascade::gui::theme module.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_UI_STYLE_COMPOSE_HPP
#define CASCADE_GUI_UI_STYLE_COMPOSE_HPP

#include "imgui.h"

namespace cascade::gui::uiscale {

// `unscaledBase` is whatever the live style looked like right after the
// current theme's own one-time setup (theme::applyTheme()) - this theme's
// colours, and ImGui-default-shaped, UN-scaled sizes. `factor` is the
// current interface scale, S. The result is that same style with every
// SIZE (WindowPadding, ItemSpacing, FramePadding, ScrollbarSize, ...)
// multiplied by S via ImGuiStyle::ScaleAllSizes (the one supported way to
// scale a whole style, applied once from an unscaled snapshot - calling it
// twice from an already-scaled style would compound the scale), and
// FontScaleMain set to S so every ordinary ImGui widget - a Button, a Combo,
// a plain Text call, anything bound to the DEFAULT font rather than an
// explicit fonts::*Px() size - grows with it too (Dear ImGui 1.92:
// "GetFontSize() == FontSizeBase * (FontScaleMain * FontScaleDpi * ...)").
// Colours pass through completely untouched - this function's only job is
// sizes, so at S=1.0 the sizes are bit-identical to `unscaledBase` and the
// colours always were.
inline ImGuiStyle composeStyle(const ImGuiStyle& unscaledBase, float factor) {
    ImGuiStyle scaled = unscaledBase;
    scaled.ScaleAllSizes(factor);
    scaled.FontScaleMain = factor;
    return scaled;
}

}  // namespace cascade::gui::uiscale

#endif  // CASCADE_GUI_UI_STYLE_COMPOSE_HPP
