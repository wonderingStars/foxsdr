// plugin_markers.hpp - where a plugin's spectrum/waterfall mark is drawn.
//
// Host API level 1 lets a plugin put marks on the spectrum and the waterfall
// (CascadeHostApi::set_marker). WHAT a mark looks like on screen - whether it
// is visible at all in this view, where its line or its band lands, what
// colour it is - is decided here, as a pure function of the mark and the view,
// so tests/test_plugin_api.cpp can pin the culling and the clipping without a
// graphics context. AppWindow::drawPluginMarkers only draws what this says.
//
// THE RULES, each tested:
//   - a mark wholly outside the view is not drawn; a span partly inside is
//     clipped to the view, never drawn past the panel's edge;
//   - CASCADE_MARKER_FLAG_SPECTRUM_ONLY keeps a mark off the waterfall;
//   - colour 0 is the host's own plugin colour; any other value is the
//     plugin's 0xRRGGBBAA, with the alpha floored so a plugin cannot put an
//     invisible mark on the user's spectrum and have it look like nothing is
//     there;
//   - a degenerate view (no span) draws nothing rather than dividing by zero.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_MARKERS_HPP
#define CASCADE_GUI_PLUGIN_MARKERS_HPP

#include <algorithm>
#include <cstdint>

#include "core/plugin_abi.h"
#include "gui/theme.hpp"
#include "imgui.h"

namespace cascade::gui {

// The least opacity a plugin's colour is drawn with.
inline constexpr std::uint32_t kPluginMarkerMinAlpha = 0x60u;

struct PluginMarkerDraw {
    bool visible = false;
    bool span = false;
    bool dashed = false;
    float x0 = 0.0f;  // a POINT's line, or a SPAN's left edge (clipped)
    float x1 = 0.0f;  // a SPAN's right edge (clipped); == x0 for a POINT
    ImU32 colour = 0;
};

// 0xRRGGBBAA as a plugin states it, to ImGui's packed colour; 0 is the host's.
inline ImU32 pluginMarkerColour(std::uint32_t rgba) {
    if (rgba == 0u) { return theme::kGold; }
    const std::uint32_t r = (rgba >> 24) & 0xFFu;
    const std::uint32_t g = (rgba >> 16) & 0xFFu;
    const std::uint32_t b = (rgba >> 8) & 0xFFu;
    const std::uint32_t a = std::max<std::uint32_t>(rgba & 0xFFu, kPluginMarkerMinAlpha);
    return IM_COL32(r, g, b, a);
}

// `lowHz`..`highHz` is the view the panel shows, `panelX`/`panelW` where the
// panel is on screen.
inline PluginMarkerDraw pluginMarkerGeometry(const CascadeMarker& m, double lowHz, double highHz,
                                             float panelX, float panelW, bool waterfall) {
    PluginMarkerDraw d;
    const double span = highHz - lowHz;
    if (!(span > 0.0) || !(panelW > 0.0f)) { return d; }
    if (waterfall && (m.flags & CASCADE_MARKER_FLAG_SPECTRUM_ONLY) != 0u) { return d; }
    const auto toX = [&](double hz) {
        return panelX + static_cast<float>((hz - lowHz) / span) * panelW;
    };
    d.colour = pluginMarkerColour(m.colourRgba);
    d.dashed = (m.flags & CASCADE_MARKER_FLAG_DASHED) != 0u;
    if (m.kind == CASCADE_MARKER_SPAN) {
        const double a = m.freqHz;
        const double b = m.freqHz + m.widthHz;
        if (!(b > lowHz) || !(a < highHz)) { return d; }
        d.span = true;
        d.x0 = toX(std::max(a, lowHz));
        d.x1 = toX(std::min(b, highHz));
        d.visible = true;
        return d;
    }
    if (!(m.freqHz >= lowHz && m.freqHz <= highHz)) { return d; }
    d.x0 = toX(m.freqHz);
    d.x1 = d.x0;
    d.visible = true;
    return d;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_MARKERS_HPP
