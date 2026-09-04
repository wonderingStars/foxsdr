// Spectrum line-plot widget — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/spectrum_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

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
//
// kBackground is the one colour here with no name in theme.hpp: the palette's
// darkest ground (kVoid) is a warm near-black for brass, and this is the cool
// near-black GREEN the three display tubes share. It stays a literal, and the
// waterfall and the scope carry the same one.
constexpr ImU32 kBackground = IM_COL32(5, 10, 6, 255);
constexpr ImU32 kTrace = theme::kPhosphor;
constexpr ImU32 kGridLine = IM_COL32(255, 255, 255, 26);

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

// The well is a recess cut into the panel, so its corners are machined, not
// square, and its bevel is the sunk one. Both numbers live here rather than at
// each call site for the reason theme.hpp gives: a frame drawn at three
// different radii stops reading as one object.
constexpr float kWellRounding = theme::kPanelRounding;
constexpr float kChromePad = 8.0f;

// Below these the annotations are not small, they are illegible and they cover
// the trace: a squeezed panel keeps its frame, its grid and its plot and drops
// the lettering rather than shipping a smear.
constexpr float kChromeMinWidth = 180.0f;
constexpr float kChromeMinHeight = 90.0f;

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

// --- lettering helpers -------------------------------------------------------
//
// Letter-spacing is drawn a glyph at a time because Dear ImGui has no tracking
// parameter, and an engraved instrument legend without it reads as a word
// rather than as a machined caption. ASCII ONLY, deliberately: a byte at a time
// is the wrong unit for UTF-8, and every caption on this face is a machine
// legend in capitals. (scope_view.cpp has the same pair for the cabinet; they
// are four lines each and live in their own file's anonymous namespace so
// neither panel can silently retune the other's lettering.)
float textWidth(ImFont* font, float px, const char* text) {
    if (font == nullptr || text == nullptr) { return 0.0f; }
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
}

float lineHeight(ImFont* font, float px) {
    if (font == nullptr) { return px; }
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, "0").y;
}

float trackedWidth(ImFont* font, float px, const char* text, float tracking) {
    if (font == nullptr || text == nullptr) { return 0.0f; }
    float w = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        w += font->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x;
        ++glyphs;
    }
    if (glyphs > 1) { w += tracking * static_cast<float>(glyphs - 1); }
    return w;
}

void addTrackedText(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
                    const char* text, float tracking) {
    if (dl == nullptr || font == nullptr || text == nullptr) { return; }
    float x = at.x;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        dl->AddText(font, px, ImVec2(x, at.y), col, one);
        x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x + tracking;
    }
}

// One frequency-ish figure with its unit, for the SPAN box. The unit is chosen
// from the magnitude and the value carries two decimals in it — the same shape
// FreqScale's own axis labels use, so the box and the axis under it never
// disagree about which unit the view is being read in.
void formatSpan(double hz, char* out, std::size_t cap) {
    const double a = std::fabs(hz);
    if (a >= 1e9) {
        std::snprintf(out, cap, "%.2f GHz", hz / 1e9);
    } else if (a >= 1e6) {
        std::snprintf(out, cap, "%.2f MHz", hz / 1e6);
    } else if (a >= 1e3) {
        std::snprintf(out, cap, "%.2f kHz", hz / 1e3);
    } else {
        std::snprintf(out, cap, "%.0f Hz", hz);
    }
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

bool SpectrumView::peakInBand(const float* dbBins, int n, double firstBin, double lastBin,
                              const VfoBand& band, float& peakDb) {
    if (dbBins == nullptr || n <= 0) { return false; }

    // Normalize the band exactly as hitTest and drawVfoOverlay do; a NaN edge
    // makes every comparison below false and lands on the "no measurement"
    // exit rather than on a bin index derived from a NaN.
    const double lo = band.x0Frac < band.x1Frac ? band.x0Frac : band.x1Frac;
    const double hi = band.x0Frac < band.x1Frac ? band.x1Frac : band.x0Frac;
    if (!(lo <= hi)) { return false; }

    double binLo = 0.0;
    double binHi = 0.0;
    if (!(lastBin > firstBin)) {
        // Degenerate window: binToXFrac has no ordering to invert. The panel
        // is showing exactly the bin at firstBin, so the question collapses to
        // "does the band touch the panel at all".
        if (!(hi >= 0.0) || !(lo <= 1.0) || !std::isfinite(firstBin)) { return false; }
        binLo = firstBin;
        binHi = firstBin;
    } else {
        // The inverse of binToXFrac. Deliberately NOT clamped to [0, 1] first:
        // a passband parked outside the zoom window still has a peak, and it
        // is a fact about the signal rather than about the view.
        binLo = firstBin + lo * (lastBin - firstBin);
        binHi = firstBin + hi * (lastBin - firstBin);
    }
    if (!std::isfinite(binLo) || !std::isfinite(binHi)) { return false; }

    const double maxBin = static_cast<double>(n - 1);
    if (binHi < 0.0 || binLo > maxBin) { return false; }  // band misses the data
    // Clamp in double BEFORE the cast: floor/ceil of a huge double is
    // undefined behaviour on conversion to int, and the band arrives
    // unclamped by contract.
    if (binLo < 0.0) { binLo = 0.0; }
    if (binHi > maxBin) { binHi = maxBin; }

    // Every WHOLE bin the interval touches, so a band narrower than one bin
    // still reports the pair it straddles instead of nothing.
    int i0 = static_cast<int>(std::floor(binLo));
    int i1 = static_cast<int>(std::ceil(binHi));
    if (i0 < 0) { i0 = 0; }
    if (i1 > n - 1) { i1 = n - 1; }

    bool any = false;
    float best = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        const float v = dbBins[i];
        // NaN loses every comparison, so it is skipped explicitly rather than
        // left to a max whose answer would depend on operand order.
        if (!(v == v)) { continue; }
        if (!any || v > best) {
            best = v;
            any = true;
        }
    }
    if (!any) { return false; }
    peakDb = best;
    return true;
}

namespace {

// --- the chrome --------------------------------------------------------------
//
// Everything lettered onto the glass, drawn after the trace so it stays
// readable over it. Returns nothing; it reports its own layout decisions
// through `headerBottom` and `axisTop`, which the dB axis then uses to keep out
// of the header's and the frequency axis's way.
//
// EVERY FIGURE HERE COMES FROM SOMETHING MEASURED. The bin count is the array
// length the caller passed, the peak is a max over the bins the passband
// actually covers, and the rest arrive in Chrome or are not drawn. There is no
// branch in this function that invents a number when an input is missing.
void drawChrome(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const float* dbBins,
                int n, double firstBin, double lastBin, const SpectrumView::Chrome* chrome,
                float& headerBottom, float& axisTop) {
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    if (w < kChromeMinWidth || h < kChromeMinHeight) { return; }

    ImFont* uiFont = fonts::ui();
    ImFont* legendFont = fonts::legend();
    ImFont* readFont = fonts::reading();
    const float tinyPx = fonts::kTinySize;
    const float legendPx = fonts::kLegendSize;
    const float readPx = fonts::kReadingSize;
    const float tinyH = lineHeight(uiFont, tinyPx);
    const float legendH = lineHeight(legendFont, legendPx);
    const float readH = lineHeight(readFont, readPx);

    const ImU32 kBright = theme::kPhosphor;
    const ImU32 kDim = theme::withAlpha(theme::kInkMuted, 0.82f);
    const ImU32 kFaint = theme::withAlpha(theme::kInkFaint, 0.85f);

    // --- header, top left ----------------------------------------------------
    // Line 1 names the picture and its parameters. The bin count is counted
    // here from what was handed over; the averaging weight only appears when
    // the caller supplied one that IS an average.
    const char* title = "SPECTRUM";
    if (chrome != nullptr && chrome->title != nullptr && chrome->title[0] != '\0') {
        title = chrome->title;
    }
    // 0 < alpha < 1 is the only state in which this trace is an average at all;
    // alpha == 1 is the estimator passing blocks straight through.
    const bool averaged = chrome != nullptr && chrome->emaAlpha > 0.0f &&
                          chrome->emaAlpha < 1.0f;

    char binPart[24] = {'\0'};
    // The count describes bins that exist: a null array with a positive count
    // is a caller bug, and printing its number would dress that up as a
    // measurement of something.
    if (dbBins != nullptr && n > 0) {
        std::snprintf(binPart, sizeof(binPart), " - %d BIN", n);
    }
    char emaPart[24] = {'\0'};
    if (averaged) {
        std::snprintf(emaPart, sizeof(emaPart), " - EMA %.2f",
                      static_cast<double>(chrome->emaAlpha));
    }
    char line1[128];
    std::snprintf(line1, sizeof(line1), "%s%s%s", title, binPart, emaPart);

    const float headX = p0.x + kChromePad;
    float headY = p0.y + kChromePad * 0.75f;
    addTrackedText(dl, legendFont, legendPx, ImVec2(headX, headY), kBright, line1, 0.7f);
    float headRight = headX + trackedWidth(legendFont, legendPx, line1, 0.7f);
    headY += legendH + 1.0f;

    // THE SECOND LINE IS A CLAIM, so it is only made when it is true. The
    // averaged trace is a computed picture of the band, not the audio being
    // demodulated, and saying so is the point of the line — but printing it
    // over an UNaveraged trace would be a false statement about the picture.
    if (averaged) {
        static const char kCaveat[] = "AVERAGE - COMPUTED, NOT HEARD";
        addTrackedText(dl, uiFont, tinyPx, ImVec2(headX, headY), kFaint, kCaveat, 0.4f);
        const float caveatRight = headX + trackedWidth(uiFont, tinyPx, kCaveat, 0.4f);
        if (caveatRight > headRight) { headRight = caveatRight; }
        headY += tinyH;
    }
    headerBottom = headY + 3.0f;

    // --- peak in the passband, top right -------------------------------------
    // Measured over the bins the caller's VFO band covers, and drawn only if
    // there IS such a measurement. Without a band there is no passband to take
    // a peak in, so the whole block stays off rather than degrading into a
    // peak over the visible width wearing this caption.
    if (chrome != nullptr && chrome->passband != nullptr) {
        float peakDb = 0.0f;
        if (SpectrumView::peakInBand(dbBins, n, firstBin, lastBin, *chrome->passband,
                                     peakDb)) {
            static const char kPeakCaption[] = "PEAK IN PASSBAND";
            char figure[24];
            std::snprintf(figure, sizeof(figure), "%.1f", static_cast<double>(peakDb));
            static const char kUnit[] = " dBFS";

            char ageLine[32] = {'\0'};
            if (chrome->dataAgeSec >= 0.0 && std::isfinite(chrome->dataAgeSec)) {
                // The age of the data the figure was measured from — not the
                // age of the GUI frame, and deliberately not "HEARD ... AGO":
                // the peak is taken off the same averaged trace the line above
                // has just called a computed picture.
                if (chrome->dataAgeSec < 10.0) {
                    std::snprintf(ageLine, sizeof(ageLine), "MEASURED %.1f s AGO",
                                  chrome->dataAgeSec);
                } else {
                    std::snprintf(ageLine, sizeof(ageLine), "MEASURED %.0f s AGO",
                                  chrome->dataAgeSec);
                }
            }

            const float capW = trackedWidth(legendFont, tinyPx, kPeakCaption, 0.7f);
            const float figW = textWidth(readFont, readPx, figure);
            const float unitW = textWidth(uiFont, tinyPx, kUnit);
            const float ageW =
                ageLine[0] == '\0' ? 0.0f : trackedWidth(uiFont, tinyPx, ageLine, 0.4f);
            const float blockW = std::max(capW, std::max(figW + unitW, ageW));

            const float right = p1.x - kChromePad;
            // Only when it clears the header. A squeezed panel loses the peak
            // block rather than overprinting it on the title.
            if (right - blockW > headRight + 12.0f) {
                float y = p0.y + kChromePad * 0.75f;
                addTrackedText(dl, legendFont, tinyPx, ImVec2(right - capW, y), kDim,
                               kPeakCaption, 0.7f);
                y += tinyH + 1.0f;
                // THE FIGURE IS AMBER AND MONOSPACED, the unit is not. Digits
                // take reading() so a changing level does not shuffle its own
                // decimal point sideways; "dBFS" is a word and takes ui(),
                // per the rule at the top of fonts.hpp.
                dl->AddText(readFont, readPx, ImVec2(right - figW - unitW, y),
                            theme::kAmber, figure);
                dl->AddText(uiFont, tinyPx, ImVec2(right - unitW, y + (readH - tinyH)),
                            theme::withAlpha(theme::kAmber, 0.85f), kUnit);
                y += readH + 1.0f;
                if (ageLine[0] != '\0') {
                    addTrackedText(dl, uiFont, tinyPx, ImVec2(right - ageW, y), kFaint,
                                   ageLine, 0.4f);
                }
            }
        }
    }

    if (chrome == nullptr) { return; }

    // --- frequency axis, along the foot --------------------------------------
    // The caller's ticks, positioned and labelled by the caller. No ticks means
    // no axis: a ruler with no numbers on it implies a scale this widget cannot
    // state.
    if (chrome->freqTicks != nullptr && chrome->freqTickCount > 0) {
        const float axisH = tinyH + 10.0f;
        const float axisY = p1.y - axisH;
        if (axisY > headerBottom + 8.0f) {
            axisTop = axisY;
            // A strip of the well's own black under the axis row, faded in
            // from above so it has no visible edge. The trace reaches the floor
            // of the panel whenever the noise sits near dbMin, and an axis
            // label with a polyline drawn through it is not dim, it is
            // unreadable. Furniture only: it darkens the ground, it does not
            // touch or reshape the trace over it.
            const ImU32 clear = theme::withAlpha(theme::kVoid, 0.0f);
            const ImU32 solid = theme::withAlpha(theme::kVoid, 0.88f);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + 1.0f, axisY - 8.0f),
                                        ImVec2(p1.x - 1.0f, axisY + 3.0f), clear, clear,
                                        solid, solid);
            dl->AddRectFilled(ImVec2(p0.x + 1.0f, axisY + 3.0f),
                              ImVec2(p1.x - 1.0f, p1.y - 1.0f), solid);
            for (int i = 0; i < chrome->freqTickCount; ++i) {
                const SpectrumView::AxisTick& t = chrome->freqTicks[i];
                // Off-panel or NaN positions are dropped rather than clamped:
                // a tick pinned to the edge would claim a frequency sits there.
                if (!(t.xFrac >= 0.0f) || !(t.xFrac <= 1.0f)) { continue; }
                const float x = p0.x + t.xFrac * w;
                dl->AddLine(ImVec2(x, axisY + 1.0f), ImVec2(x, axisY + 5.0f), kFaint);
                if (t.label == nullptr || t.label[0] == '\0') { continue; }
                const float lw = textWidth(uiFont, tinyPx, t.label);
                float lx = x - lw * 0.5f;
                // The end labels slide inboard so they are not sliced by the
                // frame; the tick itself stays put, so the mark is still true.
                if (lx < p0.x + 3.0f) { lx = p0.x + 3.0f; }
                if (lx + lw > p1.x - 3.0f) { lx = p1.x - 3.0f - lw; }
                dl->AddText(uiFont, tinyPx, ImVec2(lx, axisY + 6.0f), kFaint, t.label);
            }
        }
    }

    // --- SPAN box, bottom right ----------------------------------------------
    if (chrome->spanHz > 0.0 && std::isfinite(chrome->spanHz)) {
        static const char kSpanCaption[] = "SPAN";
        char spanText[32];
        formatSpan(chrome->spanHz, spanText, sizeof(spanText));
        const float capW = trackedWidth(legendFont, tinyPx, kSpanCaption, 0.7f);
        const float valW = textWidth(uiFont, tinyPx, spanText);
        const float boxW = std::max(capW, valW) + 12.0f;
        const float boxH = tinyH * 2.0f + 9.0f;
        const ImVec2 br(p1.x - kChromePad, axisTop - 4.0f);
        const ImVec2 tl(br.x - boxW, br.y - boxH);
        if (tl.x > p0.x + w * 0.45f && tl.y > headerBottom) {
            dl->AddRectFilled(tl, br, theme::withAlpha(theme::kVoid, 0.85f), 2.0f);
            dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.95f), 2.0f, 0, 1.0f);
            addTrackedText(dl, legendFont, tinyPx, ImVec2(tl.x + 6.0f, tl.y + 3.0f), kDim,
                           kSpanCaption, 0.7f);
            // A reading, so amber; carrying its unit, so the UI face.
            dl->AddText(uiFont, tinyPx, ImVec2(tl.x + 6.0f, tl.y + 3.0f + tinyH),
                        theme::kAmber, spanText);
        }
    }
}

}  // namespace

void SpectrumView::draw(const float* dbBins, int n, float width, float height,
                        const Chrome* chrome) {
    // Full range is just the [0, n-1] window. n == 1 collapses to the
    // zero-width window, which drawBinRange renders as the same flat line
    // this function always produced; n <= 0 draws the empty panel.
    const double lastBin = n > 0 ? static_cast<double>(n - 1) : 0.0;
    drawBinRange(dbBins, n, 0.0, lastBin, width, height, chrome);
}

void SpectrumView::drawBinRange(const float* dbBins, int n, double firstBin,
                                double lastBin, float width, float height,
                                const Chrome* chrome) {
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

    // The ground, twice: square in the void black so a rounded corner can
    // never show the window behind it, then the tube's near-black green with
    // the well's own radius. Callers rely on this panel being opaque (the
    // band-plan overlay is painted OVER the trace precisely because of it).
    drawList->AddRectFilled(p0, p1, theme::kVoid, 0.0f);
    drawList->AddRectFilled(p0, p1, kBackground, kWellRounding);
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

    // The lettering, over the trace so it stays readable, and it hands back
    // the two horizontal bands it has claimed so the dB axis can avoid them.
    float headerBottom = p0.y;
    float axisTop = p1.y;
    drawChrome(drawList, p0, p1, dbBins, n, firstBin, lastBin, chrome, headerBottom,
               axisTop);

    // The dB axis, down the left edge, from the view range this widget already
    // holds. Each label sits just below its own line so the topmost (dbMax)
    // one stays inside the panel instead of being clipped away, and a label
    // that would land under the header or in the frequency axis is dropped
    // rather than overprinted.
    const float dbLabelH = lineHeight(fonts::reading(), fonts::kTinySize);
    for (int i = 0; i < gridCount; ++i) {
        const float y = dbToY(gridDb[i], dbMin_, dbMax_, p0.y, p1.y) + 2.0f;
        if (y < headerBottom || y + dbLabelH > axisTop) { continue; }
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f", static_cast<double>(gridDb[i]));
        // Figures, so the monospaced face: a dB ladder whose digits are all
        // the same width reads as a scale rather than as a column of words.
        drawList->AddText(fonts::reading(), fonts::kTinySize,
                          ImVec2(p0.x + kChromePad, y),
                          theme::withAlpha(theme::kInkMuted, 0.78f), label);
    }

    drawList->PopClipRect();

    // The frame LAST, so nothing painted inside the well sits on top of it:
    // a brass hairline with the sunk bevel just inside it, which is what makes
    // this read as a well cut into the panel rather than a dark rectangle
    // placed on it.
    drawList->AddRect(p0, p1, theme::withAlpha(theme::kBrassDark, 0.95f), kWellRounding,
                      0, theme::kHairline);
    addBenchBevel(drawList, ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                  ImVec2(p1.x - 1.0f, p1.y - 1.0f), kWellRounding, false);

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
