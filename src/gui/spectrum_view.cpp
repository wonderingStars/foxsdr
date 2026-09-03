// Spectrum line-plot widget — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
// THE GLASS AND WHAT IS BEHIND IT, from the 1960s bench reference. The trace
// was a cool blue on near-black - a perfectly good spectrum and the wrong
// instrument. It is phosphor now, on the same near-black green the waterfall
// and the radar scope sit on, so all three displays in this product read as
// the same tube rather than three different ones.
//
// The grid stays a low-alpha WHITE rather than becoming green: a green
// graticule under a green trace is much less separable than a neutral one,
// and the graticule's whole job is to be legible without competing.
constexpr ImU32 kBackground = IM_COL32(5, 10, 6, 255);
constexpr ImU32 kTrace = IM_COL32(0x8F, 0xD9, 0xA0, 255);
constexpr ImU32 kGridLine = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kGridLabel = IM_COL32(0x9C, 0x90, 0x78, 200);

// VFO overlay: fill translucent enough that the trace stays readable through
// it, edges brighter so the grab targets are visible, and a warm center line
// that cannot be confused with the cool blue trace. Dragging brightens the
// fill so the user gets immediate "you have it" feedback.
constexpr ImU32 kVfoFill = IM_COL32(255, 255, 255, 28);
constexpr ImU32 kVfoFillDragging = IM_COL32(255, 255, 255, 52);
constexpr ImU32 kVfoEdge = IM_COL32(255, 255, 255, 140);
// The centre line takes the bench's amber, which is this product's colour for
// a figure - and is still the one warm mark on a cool face, which is why it
// could never be confused with the trace.
constexpr ImU32 kVfoCenter = IM_COL32(0xF0, 0xA8, 0x40, 200);

// Trace value at fractional bin `bin`, linearly interpolated between the two
// straddled bins. Callers guarantee bin is within [0, n-1] and n >= 1. An
// integer position returns that bin exactly (no interpolation dust), so a
// window cut on a whole bin reproduces draw()'s vertex bit-for-bit. A NaN
// bin value propagates NaN, which dbToY then pins to the panel floor — the
// same poisoned-bin policy as the unzoomed path.
float binValueAt(const float* bins, int n, double bin) {
    const int i0 = static_cast<int>(bin);
    if (i0 >= n - 1) {
        return bins[n - 1];  // bin == n-1 exactly (or clamp dust just past it)
    }
    const double frac = bin - static_cast<double>(i0);
    if (frac <= 0.0) {
        return bins[i0];
    }
    return static_cast<float>(static_cast<double>(bins[i0]) * (1.0 - frac) +
                              static_cast<double>(bins[i0 + 1]) * frac);
}

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

float SpectrumView::binToXFrac(double bin, double firstBin, double lastBin) {
    // !(lastBin > firstBin) so NaN bounds take the degenerate exit too (NaN
    // comparisons are false). 0 keeps a degenerate window's vertices on the
    // left edge instead of dividing by zero.
    if (!(lastBin > firstBin)) { return 0.0f; }
    return static_cast<float>((bin - firstBin) / (lastBin - firstBin));
}

void SpectrumView::draw(const float* dbBins, int n, float width, float height) {
    // Full range is just the [0, n-1] window. n == 1 collapses to the
    // zero-width window, which drawBinRange renders as the same flat line
    // this function always produced; n <= 0 draws the empty panel.
    const double lastBin = n > 0 ? static_cast<double>(n - 1) : 0.0;
    drawBinRange(dbBins, n, 0.0, lastBin, width, height);
}

void SpectrumView::drawBinRange(const float* dbBins, int n, double firstBin,
                                double lastBin, float width, float height) {
    // A squeezed layout can hand us a zero/negative panel; drawing into it
    // asserts inside ImGui, so skip the frame entirely (no Dummy either — a
    // negative-size item corrupts the layout cursor).
    if (width < 1.0f || height < 1.0f) { return; }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);

    // Remember the panel rect for drawVfoOverlay: the Dummy below advances
    // the layout cursor past the panel, so the overlay cannot recover this
    // rectangle from ImGui state afterwards.
    panelX_ = p0.x;
    panelY_ = p0.y;
    panelValid_ = true;

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

    // Trace. The window is interpreted per the header contract: inverted or
    // NaN -> no trace; zero-width -> flat line; otherwise the intersection
    // with the data range [0, n-1] renders, edge vertices interpolated.
    if (dbBins != nullptr && n > 0 && lastBin >= firstBin) {
        if (lastBin == firstBin) {
            // Zero-width window: one interpolated level across the whole
            // panel — the windowed analogue of the old n == 1 special case.
            double b = firstBin;
            if (b < 0.0) { b = 0.0; }
            if (b > static_cast<double>(n - 1)) { b = static_cast<double>(n - 1); }
            const float y = dbToY(binValueAt(dbBins, n, b), dbMin_, dbMax_, p0.y, p1.y);
            const ImVec2 flat[2] = {ImVec2(p0.x, y), ImVec2(p1.x, y)};
            drawList->AddPolyline(flat, 2, kTrace, ImDrawFlags_None, 1.5f);
        } else {
            // Visible data span: the window clipped to real bins. A window
            // hanging past the data leaves that part of the panel empty
            // rather than inventing bins beyond the edge.
            const double visLo = firstBin > 0.0 ? firstBin : 0.0;
            const double visHi =
                lastBin < static_cast<double>(n - 1) ? lastBin : static_cast<double>(n - 1);
            if (visHi > visLo) {
                std::vector<ImVec2> points;
                points.reserve(static_cast<std::size_t>(visHi - visLo) + 3);
                // Left cut vertex: interpolated exactly at the window edge.
                points.push_back(ImVec2(
                    p0.x + binToXFrac(visLo, firstBin, lastBin) * width,
                    dbToY(binValueAt(dbBins, n, visLo), dbMin_, dbMax_, p0.y, p1.y)));
                // Whole bins strictly inside the cut points (the cuts already
                // carry the boundary values, including exact whole-bin cuts).
                int i = static_cast<int>(std::ceil(visLo));
                if (static_cast<double>(i) <= visLo) { ++i; }
                for (; static_cast<double>(i) < visHi; ++i) {
                    // dbToY clamps, so out-of-range bins ride the panel edges
                    // rather than drawing outside the clip rect.
                    const float y = dbToY(dbBins[i], dbMin_, dbMax_, p0.y, p1.y);
                    points.push_back(
                        ImVec2(p0.x + binToXFrac(static_cast<double>(i), firstBin, lastBin) * width, y));
                }
                // Right cut vertex.
                points.push_back(ImVec2(
                    p0.x + binToXFrac(visHi, firstBin, lastBin) * width,
                    dbToY(binValueAt(dbBins, n, visHi), dbMin_, dbMax_, p0.y, p1.y)));
                drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                                      kTrace, ImDrawFlags_None, 1.5f);
            }
            // visHi <= visLo: the window touches the data at no more than a
            // single point — nothing with x extent to draw.
        }
    }

    drawList->PopClipRect();
    // Advance the layout cursor so the widget occupies its rectangle like any
    // other ImGui item and the caller can stack panels below it.
    ImGui::Dummy(ImVec2(width, height));
}

void SpectrumView::drawVfoOverlay(const VfoBand& band, float width, float height) {
    if (width < 1.0f || height < 1.0f) { return; }

    // Normalize and clamp the band into the panel. The !(...) forms route
    // NaN through the "nothing to draw" exit instead of into draw coords.
    double lo = band.x0Frac < band.x1Frac ? band.x0Frac : band.x1Frac;
    double hi = band.x0Frac < band.x1Frac ? band.x1Frac : band.x0Frac;
    if (!(hi >= 0.0) || !(lo <= 1.0)) { return; }
    if (lo < 0.0) { lo = 0.0; }
    if (hi > 1.0) { hi = 1.0; }

    // Paint over the rect recorded by the last draw()/drawBinRange(); its
    // Dummy already advanced the cursor past the panel, so the current
    // cursor is only a fallback for a caller compositing without a panel.
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (panelValid_) { p0 = ImVec2(panelX_, panelY_); }
    const ImVec2 p1(p0.x + width, p0.y + height);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(p0, p1, true);
    const float xLo = p0.x + static_cast<float>(lo) * width;
    const float xHi = p0.x + static_cast<float>(hi) * width;
    drawList->AddRectFilled(ImVec2(xLo, p0.y), ImVec2(xHi, p1.y),
                            band.dragging ? kVfoFillDragging : kVfoFill);
    // Edge lines even for a zero-width band: a collapsed VFO must stay
    // visible (and grabbable per hitTest) instead of vanishing.
    drawList->AddLine(ImVec2(xLo, p0.y), ImVec2(xLo, p1.y), kVfoEdge);
    drawList->AddLine(ImVec2(xHi, p0.y), ImVec2(xHi, p1.y), kVfoEdge);
    const float xCenter = 0.5f * (xLo + xHi);
    drawList->AddLine(ImVec2(xCenter, p0.y), ImVec2(xCenter, p1.y), kVfoCenter);
    drawList->PopClipRect();
    // No layout advance: the overlay shares the rectangle the spectrum item
    // already occupies; a second Dummy would double-space the layout.
}

SpectrumView::VfoHit SpectrumView::hitTest(double mouseXFrac, const VfoBand& band,
                                           double edgeToleranceFrac) {
    // Normalize band order; NaN in either coordinate makes lo/hi unusable
    // and every comparison below false, which lands on None as documented.
    const double lo = band.x0Frac < band.x1Frac ? band.x0Frac : band.x1Frac;
    const double hi = band.x0Frac < band.x1Frac ? band.x1Frac : band.x0Frac;
    // NaN/negative tolerance -> 0 (exact hits only); written !(tol > 0) so
    // NaN takes this branch.
    double tol = edgeToleranceFrac;
    if (!(tol > 0.0)) { tol = 0.0; }

    const double dLo = std::fabs(mouseXFrac - lo);
    const double dHi = std::fabs(mouseXFrac - hi);
    const bool nearLo = dLo <= tol;
    const bool nearHi = dHi <= tol;
    if (nearLo && nearHi) {
        // Band narrower than 2*tol (or zero-width): nearer edge wins so
        // both edges of a narrow band stay individually grabbable. Exact
        // tie resolves by approach side — a collapsed band pulls open in
        // the direction the mouse came from.
        if (dLo < dHi) { return VfoHit::EdgeLow; }
        if (dHi < dLo) { return VfoHit::EdgeHigh; }
        return mouseXFrac <= lo ? VfoHit::EdgeLow : VfoHit::EdgeHigh;
    }
    if (nearLo) { return VfoHit::EdgeLow; }
    if (nearHi) { return VfoHit::EdgeHigh; }
    // Edges checked first: within tolerance an edge beats Center, otherwise
    // a narrow band could never be resized, only moved.
    if (mouseXFrac >= lo && mouseXFrac <= hi) { return VfoHit::Center; }
    return VfoHit::None;
}

}  // namespace cascade::gui
