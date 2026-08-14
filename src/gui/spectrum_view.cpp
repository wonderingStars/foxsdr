// Spectrum line-plot widget — implementation.
//
// SPDX-License-Identifier: MIT
#include "gui/spectrum_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

namespace cascade::gui {

namespace {

// Same background as the P0 placeholder panel so swapping the real widget in
// causes no visual jump; trace color likewise carried over.
constexpr ImU32 kBackground = IM_COL32(8, 10, 14, 255);
constexpr ImU32 kTrace = IM_COL32(94, 189, 255, 255);
constexpr ImU32 kGridLine = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kGridLabel = IM_COL32(255, 255, 255, 110);

}  // namespace

float dbToY(float db, float dbMin, float dbMax, float yTop, float yBottom) {
    // Written as !(dbMax > dbMin) rather than (dbMin >= dbMax) so a NaN
    // endpoint also takes the degenerate branch (NaN comparisons are false).
    if (!(dbMax > dbMin)) { return yBottom; }
    // Fraction of the way DOWN from the top edge: 0 at dbMax, 1 at dbMin.
    // Clamping the fraction (not the output) keeps the map correct whichever
    // way yTop/yBottom are ordered numerically — screen coordinates grow
    // downward, but the function does not assume that. Clamps are spelled
    // with negated comparisons instead of std::clamp so a NaN db (a poisoned
    // bin) falls to the yBottom edge like every other unrepresentable input,
    // rather than leaking NaN into draw coordinates.
    float t = (dbMax - db) / (dbMax - dbMin);
    if (!(t < 1.0f)) {
        t = 1.0f;  // t >= 1 and NaN both land here
    } else if (t < 0.0f) {
        t = 0.0f;
    }
    return yTop + t * (yBottom - yTop);
}

int SpectrumView::gridlineDbs(float dbMin, float dbMax, float* out, int cap) {
    if (out == nullptr || cap <= 0 || !(dbMin <= dbMax)) { return 0; }
    // First/last multiples of 10 inside the range. ceil/floor in double so a
    // boundary that IS a multiple of 10 (exactly representable in float) is
    // included, satisfying the boundary-inclusive contract.
    const int kFirst = static_cast<int>(std::ceil(static_cast<double>(dbMin) / 10.0));
    const int kLast = static_cast<int>(std::floor(static_cast<double>(dbMax) / 10.0));
    int written = 0;
    for (int k = kFirst; k <= kLast && written < cap; ++k) {
        out[written++] = static_cast<float>(k * 10);
    }
    return written;
}

void SpectrumView::setRange(float dbMin, float dbMax) {
    dbMin_ = dbMin;
    dbMax_ = dbMax;
}

void SpectrumView::draw(const float* dbBins, int n, float width, float height) {
    // A squeezed layout can hand us a zero/negative panel; drawing into it
    // asserts inside ImGui, so skip the frame entirely (no Dummy either — a
    // negative-size item corrupts the layout cursor).
    if (width < 1.0f || height < 1.0f) { return; }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);

    drawList->AddRectFilled(p0, p1, kBackground);
    // Clip so gridline labels near the edges truncate instead of bleeding
    // into neighboring panels.
    drawList->PushClipRect(p0, p1, true);

    // 64 slots at one line per 10 dB covers a 640 dB span — far beyond any
    // meaningful display range, and gridlineDbs caps safely if exceeded.
    float gridDb[64];
    const int gridCount = gridlineDbs(dbMin_, dbMax_, gridDb, 64);
    for (int i = 0; i < gridCount; ++i) {
        const float y = dbToY(gridDb[i], dbMin_, dbMax_, p0.y, p1.y);
        drawList->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), kGridLine);
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(gridDb[i]));
        // Label sits just below its line so the topmost (dbMax) label stays
        // inside the panel instead of being clipped away.
        drawList->AddText(ImVec2(p0.x + 4.0f, y + 2.0f), kGridLabel, label);
    }

    if (dbBins != nullptr && n > 0) {
        std::vector<ImVec2> points;
        if (n == 1) {
            // One bin has no x extent to spread across the width; a flat
            // full-width line at its level is the least surprising rendering.
            const float y = dbToY(dbBins[0], dbMin_, dbMax_, p0.y, p1.y);
            points.push_back(ImVec2(p0.x, y));
            points.push_back(ImVec2(p1.x, y));
        } else {
            points.reserve(static_cast<std::size_t>(n));
            const float xStep = width / static_cast<float>(n - 1);
            for (int i = 0; i < n; ++i) {
                // dbToY clamps, so out-of-range bins ride the panel edges
                // rather than drawing outside the clip rect.
                const float y = dbToY(dbBins[i], dbMin_, dbMax_, p0.y, p1.y);
                points.push_back(ImVec2(p0.x + static_cast<float>(i) * xStep, y));
            }
        }
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), kTrace,
                              ImDrawFlags_None, 1.5f);
    }

    drawList->PopClipRect();
    // Advance the layout cursor so the widget occupies its rectangle like any
    // other ImGui item and the caller can stack panels below it.
    ImGui::Dummy(ImVec2(width, height));
}

}  // namespace cascade::gui
